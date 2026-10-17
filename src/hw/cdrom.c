/*
 * CD-ROM controller (0x1F801800-3): command/response protocol with queued
 * interrupts, sector reads at 1x/2x timing, XA-ADPCM streaming audio and
 * CD-DA playback (decoded into a 44.1 kHz stereo ring buffer that the SPU
 * mixes in).
 */
#include <stdlib.h>

#include "psx.h"

#define SECTOR_CYCLES_1X (PSX_CPU_HZ / 75)
#define ACK_DELAY 25000
#define INT2_DELAY 120000

enum {
    STAT_ERROR = 0x01, STAT_MOTOR = 0x02, STAT_SEEKERR = 0x04,
    STAT_IDERR = 0x08, STAT_SHELL = 0x10, STAT_READ = 0x20,
    STAT_SEEK = 0x40, STAT_PLAY = 0x80,
};

typedef struct {
    uint8_t type;      /* INT1..INT5 */
    uint8_t n;
    uint8_t bytes[16];
    int64_t at;
    bool has_sector;   /* INT1 for a data sector */
} CdIrq;

static struct {
    uint8_t index;
    uint8_t param[16];
    int nparam;
    uint8_t resp[16];
    int nresp, resp_pos;
    uint8_t int_enable, int_flag;
    CdIrq queue[8];
    int nqueue;
    bool busy;

    uint8_t stat;
    uint8_t mode;
    uint32_t setloc_lba;
    bool setloc_pending;
    uint32_t lba;          /* next sector to read/play */
    bool reading, playing;
    uint8_t filter_file, filter_chan;
    bool muted, adpcm_muted;

    uint8_t last_raw[2352];      /* last sector read (for GetlocL) */
    uint8_t pending_raw[2352];   /* sector awaiting INT1 delivery */
    uint8_t buf_raw[2352];       /* sector exposed to the data FIFO */
    bool buf_valid;
    uint8_t fifo[2352];
    int fifo_len, fifo_pos;

    uint8_t vol_pend[4], vol[4]; /* LL, LR, RL, RR */
    int play_track;
    int report_count;
} cd;

/* ------------------------------------------------------------------ */
/* audio ring buffer (44.1 kHz stereo)                                 */

#define AUDIO_RING 32768
static int16_t g_ring[AUDIO_RING * 2];
static int g_ring_r, g_ring_w;

static int ring_count(void) { return (g_ring_w - g_ring_r + AUDIO_RING) % AUDIO_RING; }

static void ring_push(int16_t l, int16_t r) {
    if (ring_count() >= AUDIO_RING - 1)
        g_ring_r = (g_ring_r + 1) % AUDIO_RING; /* drop oldest */
    g_ring[g_ring_w * 2] = l;
    g_ring[g_ring_w * 2 + 1] = r;
    g_ring_w = (g_ring_w + 1) % AUDIO_RING;
}

static inline int16_t sat16(int v) {
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

int cdrom_audio_pull(int16_t *out, int frames) {
    int n = 0;
    for (; n < frames && ring_count() > 0; n++) {
        int l = g_ring[g_ring_r * 2], r = g_ring[g_ring_r * 2 + 1];
        /* CD volume matrix (0x80 = 100%) */
        int ol = (l * cd.vol[0] + r * cd.vol[2]) >> 7;
        int orr = (l * cd.vol[1] + r * cd.vol[3]) >> 7;
        out[n * 2] = sat16(ol);
        out[n * 2 + 1] = sat16(orr);
        g_ring_r = (g_ring_r + 1) % AUDIO_RING;
    }
    for (int k = n; k < frames; k++)
        out[k * 2] = out[k * 2 + 1] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* XA-ADPCM decoder                                                    */

static int32_t xa_old[2][2]; /* per channel: [s1, s2] */
static const int xa_k0[4] = {0, 60, 115, 98};
static const int xa_k1[4] = {0, 0, -52, -55};

/* resampler state: input rate -> 44100 */
static double xa_phase;
static int16_t xa_prev[2], xa_cur[2];

static void xa_output(int16_t l, int16_t r, int rate) {
    /* linear interpolation resampler */
    xa_prev[0] = xa_cur[0];
    xa_prev[1] = xa_cur[1];
    xa_cur[0] = l;
    xa_cur[1] = r;
    double step = (double)rate / 44100.0;
    while (xa_phase < 1.0) {
        double t = xa_phase;
        int16_t ol = (int16_t)(xa_prev[0] + (xa_cur[0] - xa_prev[0]) * t);
        int16_t orr = (int16_t)(xa_prev[1] + (xa_cur[1] - xa_prev[1]) * t);
        ring_push(ol, orr);
        xa_phase += step;
    }
    xa_phase -= 1.0;
}

static void xa_decode_sector(const uint8_t *raw) {
    uint8_t coding = raw[16 + 3];
    bool stereo = (coding & 3) == 1;
    int rate = (coding & 0x0C) ? 18900 : 37800;
    bool eight = (coding & 0x30) == 0x10;
    if (eight) {
        LOGW("cd", "8-bit XA audio not supported");
        return;
    }
    const uint8_t *data = raw + 24;
    int16_t mono[224], left[112], right[112];
    for (int p = 0; p < 18; p++) {
        const uint8_t *g = data + p * 128;
        int nl = 0, nr = 0, nm = 0;
        for (int unit = 0; unit < 8; unit++) {
            uint8_t hdr = g[4 + unit];
            int shift = hdr & 0x0F;
            if (shift > 12)
                shift = 9;
            int filt = (hdr >> 4) & 3;
            int ch = stereo ? (unit & 1) : 0;
            for (int j = 0; j < 28; j++) {
                uint8_t byte = g[16 + j * 4 + unit / 2];
                int nib = (unit & 1) ? (byte >> 4) : (byte & 0xF);
                int32_t s = (int16_t)(nib << 12) >> shift;
                s += (xa_old[ch][0] * xa_k0[filt] + xa_old[ch][1] * xa_k1[filt] + 32) >> 6;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                xa_old[ch][1] = xa_old[ch][0];
                xa_old[ch][0] = s;
                if (stereo) {
                    if (ch == 0) left[nl++] = (int16_t)s;
                    else right[nr++] = (int16_t)s;
                } else {
                    mono[nm++] = (int16_t)s;
                }
            }
        }
        if (cd.muted || cd.adpcm_muted)
            continue;
        if (stereo) {
            for (int i = 0; i < 112; i++)
                xa_output(left[i], right[i], rate);
        } else {
            for (int i = 0; i < 224; i++)
                xa_output(mono[i], mono[i], rate);
        }
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0xF); }

static void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f) {
    uint32_t a = lba + 150;
    *m = bcd((int)(a / (75 * 60)));
    *s = bcd((int)((a / 75) % 60));
    *f = bcd((int)(a % 75));
}

static int64_t sector_period(void) {
    return (cd.mode & 0x80) ? SECTOR_CYCLES_1X / 2 : SECTOR_CYCLES_1X;
}

static void update_irq_line(void) {
    if (cd.int_flag & cd.int_enable & 0x1F)
        irq_raise(IRQ_CDROM);
}

static void queue_schedule(void) {
    if (cd.nqueue == 0 || cd.int_flag) {
        sched_cancel(EV_CDROM);
        return;
    }
    int64_t t = cd.queue[0].at;
    if (t < now_cycles() + 50)
        t = now_cycles() + 50;
    sched_at(EV_CDROM, t);
}

static void push_irq(uint8_t type, const uint8_t *bytes, int n, int64_t delay,
                     bool sector) {
    if (cd.nqueue >= 8) {
        LOGW("cd", "interrupt queue overflow");
        return;
    }
    CdIrq *q = &cd.queue[cd.nqueue++];
    q->type = type;
    q->n = (uint8_t)n;
    memcpy(q->bytes, bytes, (size_t)n);
    q->at = now_cycles() + delay;
    q->has_sector = sector;
    queue_schedule();
}

static void deliver(int64_t now, int64_t late) {
    (void)now, (void)late;
    if (cd.int_flag || cd.nqueue == 0)
        return;
    CdIrq q = cd.queue[0];
    memmove(cd.queue, cd.queue + 1, sizeof(CdIrq) * (size_t)(--cd.nqueue));
    cd.int_flag = q.type;
    memcpy(cd.resp, q.bytes, q.n);
    cd.nresp = q.n;
    cd.resp_pos = 0;
    if (q.has_sector) {
        memcpy(cd.buf_raw, cd.pending_raw, 2352);
        cd.buf_valid = true;
    }
    cd.busy = false;
    LOGD("cd", "deliver INT%d (%d bytes) enable=%02X", q.type, q.n, cd.int_enable);
    update_irq_line();
}

/* ------------------------------------------------------------------ */
/* reading / playing                                                   */

static void stop_reading(void) {
    cd.reading = false;
    cd.playing = false;
    cd.stat &= (uint8_t)~(STAT_READ | STAT_PLAY | STAT_SEEK);
    sched_cancel(EV_CDROM_READ);
    /* drop undelivered sector interrupts */
    int k = 0;
    for (int i = 0; i < cd.nqueue; i++)
        if (!cd.queue[i].has_sector)
            cd.queue[k++] = cd.queue[i];
    cd.nqueue = k;
}

static void read_tick(int64_t now, int64_t late) {
    (void)now, (void)late;
    if (!cd.reading && !cd.playing)
        return;
    uint8_t raw[2352];
    disc_read_sector(cd.lba, raw);
    cd.stat &= (uint8_t)~STAT_SEEK;

    if (cd.playing) {
        if (g_log_level >= LOG_DBG && (cd.lba % 75) == 0)
            LOGD("cd", "playing lba %u muted=%d vol=%02X/%02X/%02X/%02X ring=%d", cd.lba, cd.muted,
                 cd.vol[0], cd.vol[1], cd.vol[2], cd.vol[3], ring_count());
        cd.stat |= STAT_PLAY;
        if (!cd.muted) {
            for (int i = 0; i < 588; i++) {
                int16_t l = (int16_t)(raw[i * 4] | (raw[i * 4 + 1] << 8));
                int16_t r = (int16_t)(raw[i * 4 + 2] | (raw[i * 4 + 3] << 8));
                ring_push(l, r);
            }
        }
        /* end of track: autopause */
        int tn = disc_track_count();
        uint32_t next_start = (cd.play_track < tn)
                                  ? disc_track_start(cd.play_track + 1)
                                  : disc_total_sectors();
        cd.lba++;
        if ((cd.mode & 0x02) && cd.lba >= next_start) {
            stop_reading();
            uint8_t b = cd.stat;
            push_irq(4, &b, 1, 100, false);
            return;
        }
        if (cd.mode & 0x04) {
            if (++cd.report_count >= 10) {
                cd.report_count = 0;
                uint8_t m, s, f;
                uint32_t rel = cd.lba - disc_track_start(cd.play_track);
                lba_to_msf(rel - 150 < rel ? rel : 0, &m, &s, &f);
                uint8_t rep[8] = {cd.stat, bcd(cd.play_track), 1, m, s, f, 0, 0};
                push_irq(1, rep, 8, 100, false);
            }
        }
        sched_in(EV_CDROM_READ, SECTOR_CYCLES_1X);
        return;
    }

    cd.stat |= STAT_READ;
    memcpy(cd.last_raw, raw, 2352);
    cd.lba++;
    bool mode2 = raw[15] == 2;
    uint8_t file = raw[16], chan = raw[17], submode = raw[18];
    LOGD("cd", "read lba %u sub=%02X mode=%02X str sec %u/%u frame %u", cd.lba - 1, submode, cd.mode,
         raw[28] | (raw[29] << 8), raw[30] | (raw[31] << 8),
         raw[32] | (raw[33] << 8) | (raw[34] << 16));
    bool xa_audio = mode2 && (submode & 0x04) && (submode & 0x20);
    if ((cd.mode & 0x40) && xa_audio) {
        /* XA-ADPCM: routed to the audio decoder, no data interrupt */
        if (!(cd.mode & 0x08) ||
            (file == cd.filter_file && chan == cd.filter_chan))
            xa_decode_sector(raw);
        if (submode & 0x80) { /* EOF */
            if (cd.mode & 0x02) {
                /* autopause at end of file */
            }
        }
    } else if ((cd.mode & 0x08) && mode2 && (submode & 0x04)) {
        /* filtered audio sector not matching: skipped */
    } else {
        /* replace an undelivered data sector (overrun) */
        for (int i = 0; i < cd.nqueue; i++) {
            if (cd.queue[i].has_sector) {
                memmove(&cd.queue[i], &cd.queue[i + 1],
                        sizeof(CdIrq) * (size_t)(cd.nqueue - i - 1));
                cd.nqueue--;
                break;
            }
        }
        memcpy(cd.pending_raw, raw, 2352);
        uint8_t b = cd.stat;
        push_irq(1, &b, 1, 0, true);
    }
    sched_in(EV_CDROM_READ, sector_period());
}

static void start_read(bool play) {
    if (cd.setloc_pending) {
        cd.lba = cd.setloc_lba;
        cd.setloc_pending = false;
    }
    cd.reading = !play;
    cd.playing = play;
    cd.stat |= STAT_SEEK;
    cd.report_count = 0;
    xa_old[0][0] = xa_old[0][1] = xa_old[1][0] = xa_old[1][1] = 0;
    /* seek time + first sector */
    sched_in(EV_CDROM_READ, sector_period() + 30000);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */

static void exec_command(uint8_t cmd) {
    uint8_t r[16];
    uint8_t *p = cd.param;
    int np = cd.nparam;
    cd.nparam = 0;
    cd.busy = true;
    LOGD("cd", "cmd %02X params %d (%02X %02X %02X)", cmd, np, p[0], p[1], p[2]);
    switch (cmd) {
    case 0x01: /* Getstat */
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        cd.stat &= (uint8_t)~STAT_SHELL;
        break;
    case 0x02: /* Setloc */
        cd.setloc_lba = (uint32_t)((unbcd(p[0]) * 60 + unbcd(p[1])) * 75 +
                                   unbcd(p[2])) - 150;
        cd.setloc_pending = true;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x03: { /* Play */
        int track = (np >= 1) ? unbcd(p[0]) : 0;
        if (track > 0) {
            cd.setloc_lba = disc_track_start(track);
            cd.setloc_pending = true;
        }
        stop_reading();
        start_read(true);
        uint32_t target = cd.setloc_pending ? cd.setloc_lba : cd.lba;
        cd.play_track = 1;
        for (int t = 1; t <= (int)disc_track_count(); t++)
            if (disc_track_start(t) <= target)
                cd.play_track = t;
        cd.stat |= STAT_PLAY | STAT_MOTOR;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    }
    case 0x04: /* Forward */
    case 0x05: /* Backward */
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x06: /* ReadN */
    case 0x1B: /* ReadS */
        stop_reading();
        cd.stat |= STAT_MOTOR;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        start_read(false);
        break;
    case 0x07: /* MotorOn */
        cd.stat |= STAT_MOTOR;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        push_irq(2, r, 1, INT2_DELAY, false);
        break;
    case 0x08: /* Stop */
        r[0] = cd.stat;
        stop_reading();
        push_irq(3, r, 1, ACK_DELAY, false);
        cd.stat &= (uint8_t)~STAT_MOTOR;
        r[0] = cd.stat;
        push_irq(2, r, 1, INT2_DELAY, false);
        break;
    case 0x09: /* Pause */
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        stop_reading();
        r[0] = cd.stat;
        push_irq(2, r, 1, (cd.mode & 0x80) ? 70000 : 140000, false);
        break;
    case 0x0A: /* Init */
        stop_reading();
        cd.mode = 0;
        cd.stat = STAT_MOTOR;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        push_irq(2, r, 1, INT2_DELAY, false);
        break;
    case 0x0B: /* Mute */
        LOGD("cd", "mute");
        cd.muted = true;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x0C: /* Demute */
        LOGD("cd", "demute");
        cd.muted = false;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x0D: /* Setfilter */
        cd.filter_file = p[0];
        cd.filter_chan = p[1];
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x0E: /* Setmode */
        cd.mode = p[0];
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x0F: /* Getparam */
        r[0] = cd.stat;
        r[1] = cd.mode;
        r[2] = 0;
        r[3] = cd.filter_file;
        r[4] = cd.filter_chan;
        push_irq(3, r, 5, ACK_DELAY, false);
        break;
    case 0x10: /* GetlocL */
        memcpy(r, cd.last_raw + 12, 8);
        push_irq(3, r, 8, ACK_DELAY, false);
        break;
    case 0x11: { /* GetlocP */
        uint32_t lba = cd.lba ? cd.lba - 1 : 0;
        int track = 1;
        for (int t = 1; t <= (int)disc_track_count(); t++)
            if (disc_track_start(t) <= lba)
                track = t;
        uint32_t rel = lba - disc_track_start(track);
        uint8_t am, as, af;
        lba_to_msf(lba, &am, &as, &af);
        r[0] = bcd(track);
        r[1] = 1;
        r[2] = bcd((int)(rel / (75 * 60)));
        r[3] = bcd((int)((rel / 75) % 60));
        r[4] = bcd((int)(rel % 75));
        r[5] = am;
        r[6] = as;
        r[7] = af;
        push_irq(3, r, 8, ACK_DELAY, false);
        break;
    }
    case 0x12: /* SetSession */
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        push_irq(2, r, 1, INT2_DELAY, false);
        break;
    case 0x13: /* GetTN */
        r[0] = cd.stat;
        r[1] = bcd(1);
        r[2] = bcd((int)disc_track_count());
        push_irq(3, r, 3, ACK_DELAY, false);
        break;
    case 0x14: { /* GetTD */
        int t = unbcd(p[0]);
        uint32_t lba = (t == 0) ? disc_total_sectors() : disc_track_start(t);
        uint8_t m, s, f;
        lba_to_msf(lba, &m, &s, &f);
        r[0] = cd.stat;
        r[1] = m;
        r[2] = s;
        push_irq(3, r, 3, ACK_DELAY, false);
        break;
    }
    case 0x15: /* SeekL */
    case 0x16: /* SeekP */
        stop_reading();
        if (cd.setloc_pending) {
            cd.lba = cd.setloc_lba;
            cd.setloc_pending = false;
        }
        cd.stat |= STAT_MOTOR;
        r[0] = cd.stat | STAT_SEEK;
        push_irq(3, r, 1, ACK_DELAY, false);
        r[0] = cd.stat;
        push_irq(2, r, 1, INT2_DELAY, false);
        break;
    case 0x19: /* Test */
        if (p[0] == 0x20) {
            r[0] = 0x94; r[1] = 0x09; r[2] = 0x19; r[3] = 0xC0;
            push_irq(3, r, 4, ACK_DELAY, false);
        } else {
            r[0] = cd.stat;
            push_irq(3, r, 1, ACK_DELAY, false);
        }
        break;
    case 0x1A: { /* GetID */
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
        push_irq(2, id, 8, 40000, false);
        break;
    }
    case 0x1C: /* Reset */
        stop_reading();
        cd.mode = 0;
        cd.stat = STAT_MOTOR;
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        break;
    case 0x1E: /* ReadTOC */
        r[0] = cd.stat;
        push_irq(3, r, 1, ACK_DELAY, false);
        push_irq(2, r, 1, INT2_DELAY, false);
        break;
    default:
        LOGW("cd", "unknown command %02X", cmd);
        r[0] = cd.stat | STAT_ERROR;
        r[1] = 0x40;
        push_irq(5, r, 2, ACK_DELAY, false);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* registers                                                           */

void cdrom_init(void) {
    memset(&cd, 0, sizeof cd);
    cd.stat = STAT_MOTOR;
    /* the BIOS boot sequence leaves all CD interrupts enabled */
    cd.int_enable = 0x1F;
    cd.vol[0] = cd.vol[3] = 0x80;
    cd.vol_pend[0] = cd.vol_pend[3] = 0x80;
    sched_register(EV_CDROM, deliver);
    sched_register(EV_CDROM_READ, read_tick);
}

uint32_t cdrom_read(uint32_t addr) {
    if (g_log_level >= LOG_DBG && (addr & 3) != 0) LOGD("cd", "read %08X idx%d", addr, cd.index);
    switch (addr & 3) {
    case 0: {
        uint8_t s = cd.index & 3;
        s |= 0 << 2;                              /* ADPBUSY */
        s |= (cd.nparam == 0) << 3;               /* param fifo empty */
        s |= (cd.nparam < 16) << 4;               /* param fifo not full */
        s |= (cd.resp_pos < cd.nresp) << 5;       /* response ready */
        s |= (cd.fifo_pos < cd.fifo_len) << 6;    /* data request */
        s |= cd.busy << 7;
        return s;
    }
    case 1: {
        uint8_t v = 0;
        if (cd.resp_pos < cd.nresp)
            v = cd.resp[cd.resp_pos++];
        return v;
    }
    case 2: {
        uint8_t v = 0;
        if (cd.fifo_pos < cd.fifo_len)
            v = cd.fifo[cd.fifo_pos++];
        return v;
    }
    default:
        if (cd.index & 1)
            return 0xE0 | cd.int_flag;
        return 0xE0 | cd.int_enable;
    }
}

static void load_fifo(void) {
    if (!cd.buf_valid) {
        cd.fifo_len = cd.fifo_pos = 0;
        return;
    }
    if (cd.mode & 0x20) {
        memcpy(cd.fifo, cd.buf_raw + 12, 0x924);
        cd.fifo_len = 0x924;
    } else {
        memcpy(cd.fifo, cd.buf_raw + 24, 0x800);
        cd.fifo_len = 0x800;
    }
    cd.fifo_pos = 0;
}

void cdrom_write(uint32_t addr, uint32_t v) {
    uint8_t b = (uint8_t)v;
    LOGD("cd", "write %08X idx%d = %02X", addr, cd.index, b);
    switch ((addr & 3) | (cd.index << 2)) {
    case 0x0: case 0x4: case 0x8: case 0xC:
        cd.index = b & 3;
        break;
    case 0x1: /* command */
        exec_command(b);
        break;
    case 0x5: /* sound map data out */
        break;
    case 0x9: /* sound map coding info */
        break;
    case 0xD: cd.vol_pend[3] = b; break; /* R->R */
    case 0x2: /* parameter */
        if (cd.nparam < 16)
            cd.param[cd.nparam++] = b;
        break;
    case 0x6: /* interrupt enable */
        cd.int_enable = b & 0x1F;
        update_irq_line();
        break;
    case 0xA: cd.vol_pend[0] = b; break; /* L->L */
    case 0xE: cd.vol_pend[2] = b; break; /* R->L */
    case 0x3: /* request register */
        if (b & 0x80) {
            if (cd.fifo_pos >= cd.fifo_len)
                load_fifo();
        } else {
            cd.fifo_len = cd.fifo_pos = 0;
        }
        break;
    case 0x7: /* interrupt flag acknowledge */
        cd.int_flag &= (uint8_t)~(b & 0x1F);
        if (b & 0x40)
            cd.nparam = 0;
        if (!cd.int_flag) {
            cd.nresp = cd.resp_pos = 0;
            queue_schedule();
        }
        break;
    case 0xB: cd.vol_pend[1] = b; break; /* L->R */
    case 0xF: /* audio volume apply */
        cd.adpcm_muted = b & 1;
        if (b & 0x20)
            memcpy(cd.vol, cd.vol_pend, 4);
        break;
    }
}

void cdrom_dma_read(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t byte = 0;
            if (cd.fifo_pos < cd.fifo_len)
                byte = cd.fifo[cd.fifo_pos++];
            w |= (uint32_t)byte << (8 * k);
        }
        mem_w32(addr + i * 4, w);
    }
}
