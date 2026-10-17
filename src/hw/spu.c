/*
 * SPU: 24 ADPCM voices with ADSR, pitch modulation, noise, reverb, CD audio
 * input, transfers and the address-match IRQ. Samples are generated in
 * small batches from the scheduler (44.1 kHz) and handed to the platform.
 */
#include <math.h>
#include <stdlib.h>

#include "psx.h"

#define SPU_RAM_SIZE 0x80000
#define CYCLES_PER_SAMPLE 768
#define BATCH 32

void platform_audio_push(const int16_t *stereo, int frames);

static uint8_t g_sram[SPU_RAM_SIZE];

typedef enum { ADSR_OFF, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE } AdsrPhase;

typedef struct {
    uint16_t vol_l, vol_r;
    uint16_t pitch;
    uint16_t start;      /* in 8-byte units */
    uint16_t adsr_lo, adsr_hi;
    int16_t adsr_level;
    uint16_t repeat;
    /* internal */
    uint32_t cur;        /* byte address of current block */
    uint32_t counter;    /* 12.12 sample position within block */
    int16_t block[28];
    int16_t hist[4];     /* last samples of previous block for interpolation */
    int32_t old1, old2;
    uint8_t flags;
    bool decoded;
    bool ignore_loop;
    AdsrPhase phase;
    int adsr_wait;
    int16_t last_out;
} Voice;

static Voice V[24];

static struct {
    uint16_t main_l, main_r, rev_l, rev_r;
    uint32_t kon, koff, pmon, non, eon, endx;
    uint16_t rev_base;
    uint16_t irq_addr;
    uint16_t xfer_addr;
    uint32_t xfer_cur;
    uint16_t ctrl, xfer_ctrl, stat;
    uint16_t cd_l, cd_r, ext_l, ext_r;
    uint16_t rev[32];
    uint32_t rev_cur;
    int32_t noise_level;
    int noise_count;
    int64_t last_time;
    int16_t rev_out_l, rev_out_r;
    bool rev_phase;
    uint16_t regs_raw[0x200]; /* raw register mirror for reads */
} S;

/* ------------------------------------------------------------------ */
/* interpolation table: 4-tap Gaussian-like kernel                     */

static int16_t g_gauss[256][4];

static void gauss_init(void) {
    for (int i = 0; i < 256; i++) {
        double t = i / 256.0;
        double d[4] = {t + 1.0, t, 1.0 - t, 2.0 - t};
        double w[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            w[k] = exp(-(d[k] * d[k]) / (2 * 0.55 * 0.55));
            sum += w[k];
        }
        for (int k = 0; k < 4; k++)
            g_gauss[i][k] = (int16_t)lrint(w[k] / sum * 32767.0);
    }
}

/* ------------------------------------------------------------------ */

static void spu_irq_check(uint32_t addr, uint32_t len) {
    if (!(S.ctrl & 0x40))
        return;
    uint32_t ia = (uint32_t)S.irq_addr * 8;
    if (ia >= addr && ia < addr + len) {
        if (!(S.stat & 0x40)) {
            S.stat |= 0x40;
            irq_raise(IRQ_SPU);
        }
    }
}

static const int k_pos[5] = {0, 60, 115, 98, 122};
static const int k_neg[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice *v) {
    const uint8_t *b = &g_sram[v->cur & (SPU_RAM_SIZE - 1)];
    spu_irq_check(v->cur & (SPU_RAM_SIZE - 1), 16);
    int shift = b[0] & 0xF;
    if (shift > 12)
        shift = 9;
    int filt = (b[0] >> 4) & 7;
    if (filt > 4)
        filt = 4;
    v->flags = b[1];
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (v->old1 * k_pos[filt] + v->old2 * k_neg[filt] + 32) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v->old2 = v->old1;
        v->old1 = s;
        v->block[i] = (int16_t)s;
    }
    if ((v->flags & 4) && !v->ignore_loop)
        v->repeat = (uint16_t)(v->cur / 8);
    v->decoded = true;
}

/* ADSR ------------------------------------------------------------- */

static void adsr_step(Voice *v) {
    if (v->phase == ADSR_OFF) {
        v->adsr_level = 0;
        return;
    }
    if (v->adsr_wait > 0) {
        v->adsr_wait--;
        return;
    }
    int shift, step;
    bool exp_mode, decrease;
    uint16_t lo = v->adsr_lo, hi = v->adsr_hi;
    switch (v->phase) {
    case ADSR_ATTACK:
        exp_mode = (lo >> 15) & 1;
        decrease = false;
        shift = (lo >> 10) & 0x1F;
        step = 7 - ((lo >> 8) & 3);
        break;
    case ADSR_DECAY:
        exp_mode = true;
        decrease = true;
        shift = ((lo >> 4) & 0xF) << 2;
        step = -8;
        break;
    case ADSR_SUSTAIN:
        exp_mode = (hi >> 15) & 1;
        decrease = (hi >> 14) & 1;
        shift = (hi >> 8) & 0x1F;
        step = decrease ? -8 + ((hi >> 6) & 3) : 7 - ((hi >> 6) & 3);
        break;
    default: /* release */
        exp_mode = (hi >> 5) & 1;
        decrease = true;
        shift = (hi & 0x1F) << 2;
        step = -8;
        break;
    }
    int cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int32_t delta = step << (11 - shift > 0 ? 11 - shift : 0);
    int32_t level = v->adsr_level;
    if (exp_mode && !decrease && level > 0x6000)
        cycles *= 4;
    if (exp_mode && decrease)
        delta = (delta * level) >> 15;
    level += delta;
    if (level > 0x7FFF) level = 0x7FFF;
    if (level < 0) level = 0;
    v->adsr_level = (int16_t)level;
    v->adsr_wait = cycles - 1;

    switch (v->phase) {
    case ADSR_ATTACK:
        if (level >= 0x7FFF) {
            v->phase = ADSR_DECAY;
            v->adsr_wait = 0;
        }
        break;
    case ADSR_DECAY: {
        int32_t sl = ((lo & 0xF) + 1) * 0x800;
        if (level <= sl) {
            v->phase = ADSR_SUSTAIN;
            v->adsr_wait = 0;
        }
        break;
    }
    case ADSR_RELEASE:
        if (level <= 0)
            v->phase = ADSR_OFF;
        break;
    default:
        break;
    }
}

static void key_on(int i) {
    Voice *v = &V[i];
    v->cur = (uint32_t)v->start * 8;
    v->counter = 0;
    v->adsr_level = 0;
    v->adsr_wait = 0;
    v->old1 = v->old2 = 0;
    memset(v->hist, 0, sizeof v->hist);
    v->decoded = false;
    v->ignore_loop = false;
    v->phase = ADSR_ATTACK;
    S.endx &= ~(1u << i);
}

static void key_off(int i) {
    Voice *v = &V[i];
    if (v->phase != ADSR_OFF) {
        v->phase = ADSR_RELEASE;
        v->adsr_wait = 0;
    }
}

static inline int16_t voice_sample_at(Voice *v, int idx) {
    /* idx in [-3, 27]: negative values come from the previous block */
    if (idx >= 0)
        return v->block[idx];
    return v->hist[4 + idx];
}

static int32_t voice_tick(int i, int32_t prev_out) {
    Voice *v = &V[i];
    if (!v->decoded)
        decode_block(v);
    int32_t sample;
    if ((S.non >> i) & 1) {
        sample = (int16_t)S.noise_level;
    } else {
        int pos = (int)(v->counter >> 12);
        int g = (v->counter >> 4) & 0xFF;
        int32_t s = 0;
        s += voice_sample_at(v, pos - 3) * g_gauss[g][0];
        s += voice_sample_at(v, pos - 2) * g_gauss[g][1];
        s += voice_sample_at(v, pos - 1) * g_gauss[g][2];
        s += voice_sample_at(v, pos) * g_gauss[g][3];
        sample = s >> 15;
    }
    adsr_step(v);
    int32_t out = (sample * v->adsr_level) >> 15;
    v->last_out = (int16_t)out;

    /* advance */
    uint32_t step = v->pitch;
    if (i > 0 && ((S.pmon >> i) & 1)) {
        int32_t f = prev_out + 0x8000;
        step = (uint32_t)(((int32_t)(int16_t)step * f) >> 15) & 0xFFFF;
    }
    if (step > 0x3FFF)
        step = 0x4000;
    v->counter += step;
    while ((v->counter >> 12) >= 28) {
        v->counter -= 28 << 12;
        /* keep last 3 samples for interpolation across blocks */
        v->hist[1] = v->block[25];
        v->hist[2] = v->block[26];
        v->hist[3] = v->block[27];
        uint8_t fl = v->flags;
        v->cur += 16;
        if (fl & 1) { /* loop end */
            S.endx |= 1u << i;
            v->cur = (uint32_t)v->repeat * 8;
            if (!(fl & 2)) { /* no repeat: voice ends */
                v->phase = ADSR_OFF;
                v->adsr_level = 0;
            }
        }
        decode_block(v);
    }
    return out;
}

static int32_t apply_vol(int32_t s, uint16_t vol) {
    int32_t v = (vol & 0x8000) ? 0x7FFF : (int16_t)(vol << 1);
    return (s * v) >> 15;
}

/* reverb ------------------------------------------------------------- */

#define RV(n) ((int16_t)S.rev[n])
enum {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
    mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME,
    mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
    mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN
};

static uint32_t rv_addr(int32_t off) {
    uint32_t base = (uint32_t)S.rev_base * 8;
    uint32_t size = SPU_RAM_SIZE - base;
    if (size == 0)
        return 0;
    int64_t a = (int64_t)S.rev_cur + off - base;
    a %= size;
    if (a < 0) a += size;
    return (uint32_t)(base + a) & (SPU_RAM_SIZE - 2);
}
static int16_t rv_rd(int32_t off) {
    uint32_t a = rv_addr(off);
    return (int16_t)(g_sram[a] | (g_sram[a + 1] << 8));
}
static void rv_wr(int32_t off, int32_t v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    uint32_t a = rv_addr(off);
    g_sram[a] = (uint8_t)v;
    g_sram[a + 1] = (uint8_t)(v >> 8);
}
static inline int32_t mul(int32_t a, int32_t b) { return (a * b) >> 15; }

static void reverb_tick(int32_t in_l, int32_t in_r) {
    if (!(S.ctrl & 0x80)) {
        S.rev_out_l = S.rev_out_r = 0;
        return;
    }
    int32_t L = mul(in_l, RV(vLIN)), Rr = mul(in_r, RV(vRIN));
#define A(r) ((int32_t)S.rev[r] * 8)
    int32_t lsame = mul(L + mul(rv_rd(A(dLSAME)), RV(vWALL)) - rv_rd(A(mLSAME) - 2), RV(vIIR)) + rv_rd(A(mLSAME) - 2);
    int32_t rsame = mul(Rr + mul(rv_rd(A(dRSAME)), RV(vWALL)) - rv_rd(A(mRSAME) - 2), RV(vIIR)) + rv_rd(A(mRSAME) - 2);
    int32_t ldiff = mul(L + mul(rv_rd(A(dRDIFF)), RV(vWALL)) - rv_rd(A(mLDIFF) - 2), RV(vIIR)) + rv_rd(A(mLDIFF) - 2);
    int32_t rdiff = mul(Rr + mul(rv_rd(A(dLDIFF)), RV(vWALL)) - rv_rd(A(mRDIFF) - 2), RV(vIIR)) + rv_rd(A(mRDIFF) - 2);
    rv_wr(A(mLSAME), lsame);
    rv_wr(A(mRSAME), rsame);
    rv_wr(A(mLDIFF), ldiff);
    rv_wr(A(mRDIFF), rdiff);
    int32_t lo = mul(RV(vCOMB1), rv_rd(A(mLCOMB1))) + mul(RV(vCOMB2), rv_rd(A(mLCOMB2))) +
                 mul(RV(vCOMB3), rv_rd(A(mLCOMB3))) + mul(RV(vCOMB4), rv_rd(A(mLCOMB4)));
    int32_t ro = mul(RV(vCOMB1), rv_rd(A(mRCOMB1))) + mul(RV(vCOMB2), rv_rd(A(mRCOMB2))) +
                 mul(RV(vCOMB3), rv_rd(A(mRCOMB3))) + mul(RV(vCOMB4), rv_rd(A(mRCOMB4)));
    lo = lo - mul(RV(vAPF1), rv_rd(A(mLAPF1) - A(dAPF1)));
    rv_wr(A(mLAPF1), lo);
    lo = mul(lo, RV(vAPF1)) + rv_rd(A(mLAPF1) - A(dAPF1));
    ro = ro - mul(RV(vAPF1), rv_rd(A(mRAPF1) - A(dAPF1)));
    rv_wr(A(mRAPF1), ro);
    ro = mul(ro, RV(vAPF1)) + rv_rd(A(mRAPF1) - A(dAPF1));
    lo = lo - mul(RV(vAPF2), rv_rd(A(mLAPF2) - A(dAPF2)));
    rv_wr(A(mLAPF2), lo);
    lo = mul(lo, RV(vAPF2)) + rv_rd(A(mLAPF2) - A(dAPF2));
    ro = ro - mul(RV(vAPF2), rv_rd(A(mRAPF2) - A(dAPF2)));
    rv_wr(A(mRAPF2), ro);
    ro = mul(ro, RV(vAPF2)) + rv_rd(A(mRAPF2) - A(dAPF2));
#undef A
    S.rev_out_l = (int16_t)(lo > 32767 ? 32767 : (lo < -32768 ? -32768 : lo));
    S.rev_out_r = (int16_t)(ro > 32767 ? 32767 : (ro < -32768 ? -32768 : ro));
    uint32_t base = (uint32_t)S.rev_base * 8;
    S.rev_cur += 2;
    if (S.rev_cur >= SPU_RAM_SIZE || S.rev_cur < base)
        S.rev_cur = base;
}

/* noise ------------------------------------------------------------- */

static void noise_tick(void) {
    int shift = (S.ctrl >> 10) & 0xF, step = ((S.ctrl >> 8) & 3) + 4;
    int timer = 0x20000 >> shift;
    S.noise_count -= step;
    if (S.noise_count < 0) {
        S.noise_count += timer;
        uint32_t l = (uint32_t)S.noise_level;
        uint32_t parity = ((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1;
        S.noise_level = (int32_t)(int16_t)((l << 1) | parity);
    }
}

/* ------------------------------------------------------------------ */

static inline int16_t clamp16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

void spu_generate(int16_t *out, int frames) {
    int16_t cdbuf[BATCH * 2];
    for (int f0 = 0; f0 < frames; f0 += BATCH) {
        int n = frames - f0 < BATCH ? frames - f0 : BATCH;
        cdrom_audio_pull(cdbuf, n);
        for (int k = 0; k < n; k++) {
            int32_t ml = 0, mr = 0, rl = 0, rr = 0;
            noise_tick();
            int32_t prev = 0;
            for (int i = 0; i < 24; i++) {
                Voice *v = &V[i];
                if (v->phase == ADSR_OFF && v->adsr_level == 0) {
                    v->last_out = 0;
                    prev = 0;
                    continue;
                }
                int32_t s = voice_tick(i, prev);
                prev = s;
                int32_t l = apply_vol(s, v->vol_l), r = apply_vol(s, v->vol_r);
                ml += l;
                mr += r;
                if ((S.eon >> i) & 1) {
                    rl += l;
                    rr += r;
                }
            }
            if (S.ctrl & 1) { /* CD audio enable */
                /* CD input volume is a plain signed 16-bit factor */
                int32_t cl = (cdbuf[k * 2] * (int16_t)S.cd_l) >> 15;
                int32_t cr = (cdbuf[k * 2 + 1] * (int16_t)S.cd_r) >> 15;
                ml += cl;
                mr += cr;
                if (S.ctrl & 4) {
                    rl += cl;
                    rr += cr;
                }
            }
            /* reverb runs at 22.05 kHz */
            S.rev_phase = !S.rev_phase;
            if (S.rev_phase)
                reverb_tick(clamp16(rl), clamp16(rr));
            ml += mul(S.rev_out_l, (int16_t)S.rev_l);
            mr += mul(S.rev_out_r, (int16_t)S.rev_r);
            ml = apply_vol(clamp16(ml), S.main_l);
            mr = apply_vol(clamp16(mr), S.main_r);
            if (!(S.ctrl & 0x4000) || !(S.ctrl & 0x8000))
                ml = mr = 0;
            out[(f0 + k) * 2] = clamp16(ml);
            out[(f0 + k) * 2 + 1] = clamp16(mr);
        }
    }
}

static void spu_event(int64_t now, int64_t late) {
    (void)late;
    int16_t buf[BATCH * 2];
    int64_t elapsed = now - S.last_time;
    int frames = (int)(elapsed / CYCLES_PER_SAMPLE);
    while (frames > 0) {
        int n = frames > BATCH ? BATCH : frames;
        spu_generate(buf, n);
        platform_audio_push(buf, n);
        frames -= n;
        S.last_time += (int64_t)n * CYCLES_PER_SAMPLE;
    }
    sched_at(EV_SPU, S.last_time + BATCH * CYCLES_PER_SAMPLE);
}

void spu_init(void) {
    memset(V, 0, sizeof V);
    memset(&S, 0, sizeof S);
    memset(g_sram, 0, sizeof g_sram);
    gauss_init();
    S.noise_level = 1;
    sched_register(EV_SPU, spu_event);
    S.last_time = 0;
    sched_at(EV_SPU, BATCH * CYCLES_PER_SAMPLE);
}

/* ------------------------------------------------------------------ */
/* registers                                                           */

uint32_t spu_read16(uint32_t addr) {
    uint32_t off = addr - 0x1F801C00;
    if (off < 0x180) {
        Voice *v = &V[off >> 4];
        switch (off & 0xF) {
        case 0x0: return v->vol_l;
        case 0x2: return v->vol_r;
        case 0x4: return v->pitch;
        case 0x6: return v->start;
        case 0x8: return v->adsr_lo;
        case 0xA: return v->adsr_hi;
        case 0xC: return (uint16_t)v->adsr_level;
        case 0xE: return v->repeat;
        }
    }
    switch (off) {
    case 0x180: return S.main_l;
    case 0x182: return S.main_r;
    case 0x184: return S.rev_l;
    case 0x186: return S.rev_r;
    case 0x188: return S.kon & 0xFFFF;
    case 0x18A: return S.kon >> 16;
    case 0x18C: return S.koff & 0xFFFF;
    case 0x18E: return S.koff >> 16;
    case 0x190: return S.pmon & 0xFFFF;
    case 0x192: return S.pmon >> 16;
    case 0x194: return S.non & 0xFFFF;
    case 0x196: return S.non >> 16;
    case 0x198: return S.eon & 0xFFFF;
    case 0x19A: return S.eon >> 16;
    case 0x19C: return S.endx & 0xFFFF;
    case 0x19E: return S.endx >> 16;
    case 0x1A2: return S.rev_base;
    case 0x1A4: return S.irq_addr;
    case 0x1A6: return S.xfer_addr;
    case 0x1A8: return 0;
    case 0x1AA: return S.ctrl;
    case 0x1AC: return S.xfer_ctrl;
    case 0x1AE: return (uint16_t)((S.stat & 0xFFC0) | (S.ctrl & 0x3F));
    case 0x1B0: return S.cd_l;
    case 0x1B2: return S.cd_r;
    case 0x1B4: return S.ext_l;
    case 0x1B6: return S.ext_r;
    case 0x1B8: return S.main_l;
    case 0x1BA: return S.main_r;
    default:
        if (off >= 0x1C0 && off < 0x200)
            return S.rev[(off - 0x1C0) >> 1];
        if (off >= 0x200 && off < 0x260) {
            Voice *v = &V[(off - 0x200) >> 2];
            return (uint16_t)v->last_out;
        }
        return S.regs_raw[(off >> 1) & 0x1FF];
    }
}

static void sram_write16(uint32_t a, uint16_t v) {
    a &= SPU_RAM_SIZE - 2;
    spu_irq_check(a, 2);
    g_sram[a] = (uint8_t)v;
    g_sram[a + 1] = (uint8_t)(v >> 8);
}

void spu_write16(uint32_t addr, uint32_t val) {
    uint16_t v = (uint16_t)val;
    uint32_t off = addr - 0x1F801C00;
    /* bring audio up to date before register changes take effect */
    spu_event(now_cycles(), 0);
    S.regs_raw[(off >> 1) & 0x1FF] = v;
    if (off < 0x180) {
        Voice *vc = &V[off >> 4];
        switch (off & 0xF) {
        case 0x0: vc->vol_l = v; break;
        case 0x2: vc->vol_r = v; break;
        case 0x4: vc->pitch = v; break;
        case 0x6: vc->start = v; break;
        case 0x8: vc->adsr_lo = v; break;
        case 0xA: vc->adsr_hi = v; break;
        case 0xC: vc->adsr_level = (int16_t)v; break;
        case 0xE: vc->repeat = v; vc->ignore_loop = true; break;
        }
        return;
    }
    switch (off) {
    case 0x180: S.main_l = v; LOGD("spu", "main vol L = %04X", v); break;
    case 0x182: S.main_r = v; LOGD("spu", "main vol R = %04X", v); break;
    case 0x184: S.rev_l = v; break;
    case 0x186: S.rev_r = v; break;
    case 0x188: for (int i = 0; i < 16; i++) if (v >> i & 1) key_on(i); S.kon = (S.kon & 0xFFFF0000u) | v; break;
    case 0x18A: for (int i = 0; i < 8; i++) if (v >> i & 1) key_on(16 + i); S.kon = (S.kon & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x18C: for (int i = 0; i < 16; i++) if (v >> i & 1) key_off(i); S.koff = (S.koff & 0xFFFF0000u) | v; break;
    case 0x18E: for (int i = 0; i < 8; i++) if (v >> i & 1) key_off(16 + i); S.koff = (S.koff & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x190: S.pmon = (S.pmon & 0xFFFF0000u) | v; break;
    case 0x192: S.pmon = (S.pmon & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x194: S.non = (S.non & 0xFFFF0000u) | v; break;
    case 0x196: S.non = (S.non & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x198: S.eon = (S.eon & 0xFFFF0000u) | v; break;
    case 0x19A: S.eon = (S.eon & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x19C: case 0x19E: break; /* ENDX read-only */
    case 0x1A2:
        S.rev_base = v;
        S.rev_cur = (uint32_t)v * 8;
        break;
    case 0x1A4: S.irq_addr = v; break;
    case 0x1A6:
        S.xfer_addr = v;
        S.xfer_cur = (uint32_t)v * 8;
        break;
    case 0x1A8: /* manual transfer FIFO */
        sram_write16(S.xfer_cur, v);
        S.xfer_cur = (S.xfer_cur + 2) & (SPU_RAM_SIZE - 1);
        break;
    case 0x1AA:
        if ((S.ctrl ^ v) & 0x8005)
            LOGD("spu", "SPUCNT = %04X", v);
        S.ctrl = v;
        if (!(v & 0x40)) {
            S.stat &= (uint16_t)~0x40; /* acknowledge IRQ */
        }
        break;
    case 0x1AC: S.xfer_ctrl = v; break;
    case 0x1AE: break;
    case 0x1B0: S.cd_l = v; LOGD("spu", "CD vol L = %04X", v); break;
    case 0x1B2: S.cd_r = v; LOGD("spu", "CD vol R = %04X", v); break;
    case 0x1B4: S.ext_l = v; break;
    case 0x1B6: S.ext_r = v; break;
    default:
        if (off >= 0x1C0 && off < 0x200)
            S.rev[(off - 0x1C0) >> 1] = v;
        break;
    }
}

void spu_dma_write(uint32_t addr, uint32_t words) {
    spu_event(now_cycles(), 0);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = mem_r32(addr + i * 4);
        sram_write16(S.xfer_cur, (uint16_t)w);
        S.xfer_cur = (S.xfer_cur + 2) & (SPU_RAM_SIZE - 1);
        sram_write16(S.xfer_cur, (uint16_t)(w >> 16));
        S.xfer_cur = (S.xfer_cur + 2) & (SPU_RAM_SIZE - 1);
    }
}

void spu_dma_read(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t a = S.xfer_cur;
        spu_irq_check(a, 4);
        uint32_t w = g_sram[a] | (g_sram[(a + 1) & (SPU_RAM_SIZE - 1)] << 8) |
                     ((uint32_t)g_sram[(a + 2) & (SPU_RAM_SIZE - 1)] << 16) |
                     ((uint32_t)g_sram[(a + 3) & (SPU_RAM_SIZE - 1)] << 24);
        mem_w32(addr + i * 4, w);
        S.xfer_cur = (a + 4) & (SPU_RAM_SIZE - 1);
    }
}
