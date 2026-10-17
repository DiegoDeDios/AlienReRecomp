/*
 * GTE (COP2) - Geometry Transformation Engine.
 *
 * Fixed-point semantics follow the documented hardware behaviour (nocash
 * psx-spx): 44-bit MAC overflow detection, IR/colour/SZ/SXY saturation with
 * FLAG bits, the UNR reciprocal used by RTPS/RTPT, and the MVMVA far-colour
 * quirk.
 */
#include "psx.h"
#include "pgxp.h"
#include "widescreen.h"

/* data register indices */
enum {
    VXY0, VZ0, VXY1, VZ1, VXY2, VZ2, RGBC, OTZ, IR0, IR1, IR2, IR3, SXY0,
    SXY1, SXY2, SXYP, SZ0, SZ1, SZ2, SZ3, RGB0, RGB1, RGB2, RES1, MAC0, MAC1,
    MAC2, MAC3, IRGB, ORGB, LZCS, LZCR
};
/* control register indices */
enum {
    RT11RT12, RT13RT21, RT22RT23, RT31RT32, RT33, TRX, TRY, TRZ, L11L12,
    L13L21, L22L23, L31L32, L33, RBK, GBK, BBK, LR1LR2, LR3LG1, LG2LG3,
    LB1LB2, LB3, RFC, GFC, BFC, OFX, OFY, H, DQA, DQB, ZSF3, ZSF4, FLAG
};

#define D (c->gte.data)
#define C (c->gte.ctrl)

static inline int32_t lo16(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline int32_t hi16(uint32_t v) { return (int16_t)(v >> 16); }

/* matrix element access: m is base ctrl index of a 3x3 matrix (0,8,16) */
static inline int32_t M(CPU *c, int m, int row, int col) {
    int idx = row * 3 + col;
    uint32_t w = C[m + idx / 2];
    return (idx & 1) ? hi16(w) : lo16(w);
}

static inline int32_t VX(CPU *c, int n) { return lo16(D[n * 2]); }
static inline int32_t VY(CPU *c, int n) { return hi16(D[n * 2]); }
static inline int32_t VZ(CPU *c, int n) { return lo16(D[n * 2 + 1]); }

static inline void set_flag(CPU *c, uint32_t bit) { C[FLAG] |= bit; }

/* --- MAC1..3: 44-bit overflow checks, value kept as 44-bit sign-extended */
static inline int64_t mac_check(CPU *c, int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL)
        set_flag(c, 1u << (31 - i)); /* MAC1 +ovf = bit30, MAC2 29, MAC3 28 */
    else if (v < -0x80000000000LL)
        set_flag(c, 1u << (28 - i)); /* MAC1 -ovf = bit27, MAC2 26, MAC3 25 */
    return (int64_t)((uint64_t)v << 20) >> 20;
}

static inline void set_mac(CPU *c, int i, int64_t v, int shift) {
    mac_check(c, i, v);
    D[MAC0 + i] = (uint32_t)(int32_t)(v >> shift);
}

static inline void set_mac0(CPU *c, int64_t v) {
    if (v > 0x7FFFFFFFLL)
        set_flag(c, 1u << 16);
    else if (v < -0x80000000LL)
        set_flag(c, 1u << 15);
    D[MAC0] = (uint32_t)(int32_t)v;
}

static inline int32_t sat_ir(CPU *c, int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) {
        set_flag(c, 1u << (25 - i)); /* IR1 bit24, IR2 23, IR3 22 */
        return lo;
    }
    if (v > 0x7FFF) {
        set_flag(c, 1u << (25 - i));
        return 0x7FFF;
    }
    return v;
}

static inline void set_ir(CPU *c, int i, int32_t v, int lm) {
    D[IR0 + i] = (uint32_t)sat_ir(c, i, v, lm);
}

static inline void macs_to_irs(CPU *c, int lm) {
    set_ir(c, 1, (int32_t)D[MAC1], lm);
    set_ir(c, 2, (int32_t)D[MAC2], lm);
    set_ir(c, 3, (int32_t)D[MAC3], lm);
}

static inline uint32_t sat_color(CPU *c, int i, int32_t v) {
    if (v < 0) { set_flag(c, 1u << (22 - i)); return 0; }  /* R bit21 */
    if (v > 255) { set_flag(c, 1u << (22 - i)); return 255; }
    return (uint32_t)v;
}

static void push_color(CPU *c) {
    uint32_t r = sat_color(c, 1, (int32_t)D[MAC1] >> 4);
    uint32_t g = sat_color(c, 2, (int32_t)D[MAC2] >> 4);
    uint32_t b = sat_color(c, 3, (int32_t)D[MAC3] >> 4);
    D[RGB0] = D[RGB1];
    D[RGB1] = D[RGB2];
    D[RGB2] = r | (g << 8) | (b << 16) | (D[RGBC] & 0xFF000000u);
}

static void push_sz(CPU *c, int64_t v) {
    if (v < 0) { set_flag(c, 1u << 18); v = 0; }
    else if (v > 0xFFFF) { set_flag(c, 1u << 18); v = 0xFFFF; }
    D[SZ0] = D[SZ1];
    D[SZ1] = D[SZ2];
    D[SZ2] = D[SZ3];
    D[SZ3] = (uint32_t)v;
}

static void push_sxy(CPU *c, int64_t x, int64_t y) {
    if (x < -0x400) { set_flag(c, 1u << 14); x = -0x400; }
    else if (x > 0x3FF) { set_flag(c, 1u << 14); x = 0x3FF; }
    if (y < -0x400) { set_flag(c, 1u << 13); y = -0x400; }
    else if (y > 0x3FF) { set_flag(c, 1u << 13); y = 0x3FF; }
    D[SXY0] = D[SXY1];
    D[SXY1] = D[SXY2];
    D[SXY2] = ((uint32_t)x & 0xFFFF) | ((uint32_t)y << 16);
    if (g_ws_enabled)
        ws_record((int32_t)x, (int32_t)y);
}

/* --- UNR division ------------------------------------------------------ */
static uint8_t unr_table[0x101];
static int unr_ready;

static void unr_init(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}

static uint32_t gte_divide(CPU *c, uint32_t h, uint32_t sz3) {
    if (h < sz3 * 2) {
        int z = __builtin_clz((uint32_t)sz3 & 0xFFFF) - 16; /* leading zeros in 16 bits */
        if (sz3 == 0) z = 16;
        uint64_t n = (uint64_t)h << z;
        uint32_t d = (uint32_t)sz3 << z;
        uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x0000080 + d * u) >> 8;
        uint64_t q = (n * d + 0x8000) >> 16;
        return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
    }
    set_flag(c, 1u << 17);
    return 0x1FFFF;
}

/* --- perspective transform of one vertex -------------------------------- */
static void rtp(CPU *c, int v, int sf, int lm, int last) {
    int shift = sf ? 12 : 0;
    int32_t vx = VX(c, v), vy = VY(c, v), vz = VZ(c, v);
    int64_t x = ((int64_t)(int32_t)C[TRX] << 12);
    x = mac_check(c, 1, x + M(c, 0, 0, 0) * vx);
    x = mac_check(c, 1, x + M(c, 0, 0, 1) * vy);
    x = mac_check(c, 1, x + M(c, 0, 0, 2) * vz);
    int64_t y = ((int64_t)(int32_t)C[TRY] << 12);
    y = mac_check(c, 2, y + M(c, 0, 1, 0) * vx);
    y = mac_check(c, 2, y + M(c, 0, 1, 1) * vy);
    y = mac_check(c, 2, y + M(c, 0, 1, 2) * vz);
    int64_t z = ((int64_t)(int32_t)C[TRZ] << 12);
    z = mac_check(c, 3, z + M(c, 0, 2, 0) * vx);
    z = mac_check(c, 3, z + M(c, 0, 2, 1) * vy);
    z = mac_check(c, 3, z + M(c, 0, 2, 2) * vz);
    D[MAC1] = (uint32_t)(int32_t)(x >> shift);
    D[MAC2] = (uint32_t)(int32_t)(y >> shift);
    D[MAC3] = (uint32_t)(int32_t)(z >> shift);
    set_ir(c, 1, (int32_t)D[MAC1], lm);
    set_ir(c, 2, (int32_t)D[MAC2], lm);
    /* IR3 quirk: the saturation flag is computed from MAC3>>12 regardless
     * of sf, while the value is saturated normally */
    {
        int32_t z12 = (int32_t)(z >> 12);
        int32_t m3 = (int32_t)D[MAC3];
        int32_t lo = lm ? 0 : -0x8000;
        if (z12 < -0x8000 || z12 > 0x7FFF)
            set_flag(c, 1u << 22);
        D[IR3] = (uint32_t)(m3 < lo ? lo : (m3 > 0x7FFF ? 0x7FFF : m3));
    }
    push_sz(c, z >> 12);
    uint32_t q = gte_divide(c, C[H] & 0xFFFF, D[SZ3]);
    int64_t px = (int64_t)q * (int16_t)D[IR1];
    if (g_ws_enabled) /* widescreen: narrower horizontal projection */
        px = (px * g_ws_k_fp) >> 16;
    int64_t sx = px + (int32_t)C[OFX];
    int64_t sy = (int64_t)q * (int16_t)D[IR2] + (int32_t)C[OFY];
    set_mac0(c, sx);
    set_mac0(c, sy);
    push_sxy(c, sx >> 16, sy >> 16);
    if (g_pgxp) {
        /* precise projection: true division instead of the UNR reciprocal,
         * no rounding to whole pixels and no saturation. This is exactly
         * where the hardware result degrades (very near the camera, where
         * its divider overflows), so those vertices are kept; only points
         * behind/at the eye or absurdly far off-screen stay untrusted. */
        double zf = (double)z / 4096.0;
        double xf = (double)x / (double)(1 << shift);
        double yf = (double)y / (double)(1 << shift);
        double hh = (double)(C[H] & 0xFFFF);
        bool ok = zf >= 1.0;
        double fx = (double)(int32_t)C[OFX] / 65536.0 + xf * hh / zf * g_ws_k;
        double fy = (double)(int32_t)C[OFY] / 65536.0 + yf * hh / zf;
        ok = ok && fx > -8192.0 && fx < 8192.0 && fy > -8192.0 && fy < 8192.0;
        pgxp_gte_push((float)fx, (float)fy, (float)zf, D[SXY2], ok);
    }
    if (last) {
        int64_t dq = (int64_t)q * (int16_t)C[DQA] + (int32_t)C[DQB];
        set_mac0(c, dq);
        int64_t ir0 = dq >> 12;
        if (ir0 < 0) { set_flag(c, 1u << 12); ir0 = 0; }
        else if (ir0 > 0x1000) { set_flag(c, 1u << 12); ir0 = 0x1000; }
        D[IR0] = (uint32_t)ir0;
    }
}

/* --- generic matrix * vector + translation ----------------------------- */
static void mvmva_core(CPU *c, int mx, const int32_t vec[3], int cv, int sf,
                       int lm) {
    int shift = sf ? 12 : 0;
    int32_t tr[3] = {0, 0, 0};
    if (cv == 0) {
        tr[0] = (int32_t)C[TRX]; tr[1] = (int32_t)C[TRY]; tr[2] = (int32_t)C[TRZ];
    } else if (cv == 1) {
        tr[0] = (int32_t)C[RBK]; tr[1] = (int32_t)C[GBK]; tr[2] = (int32_t)C[BBK];
    } else if (cv == 2) {
        tr[0] = (int32_t)C[RFC]; tr[1] = (int32_t)C[GFC]; tr[2] = (int32_t)C[BFC];
    }
    for (int i = 0; i < 3; i++) {
        int32_t m0, m1, m2;
        if (mx == 3) {
            /* garbage matrix: row = [-R<<4, R<<4, IR0], all rows use RT13/RT22 */
            int32_t r = (int32_t)(D[RGBC] & 0xFF);
            m0 = -(r << 4);
            m1 = r << 4;
            m2 = (int16_t)D[IR0];
            if (i == 1) { m0 = M(c, 0, 0, 2); m1 = M(c, 0, 0, 2); m2 = M(c, 0, 0, 2); }
            if (i == 2) { m0 = M(c, 0, 1, 1); m1 = M(c, 0, 1, 1); m2 = M(c, 0, 1, 1); }
        } else {
            int base = mx * 8;
            m0 = M(c, base, i, 0);
            m1 = M(c, base, i, 1);
            m2 = M(c, base, i, 2);
        }
        int64_t acc = (int64_t)tr[i] << 12;
        if (cv == 2) {
            /* far colour bug: the first column product only affects
             * flags; the result uses columns 2 and 3 */
            int64_t t = mac_check(c, i + 1, acc + (int64_t)m0 * vec[0]);
            sat_ir(c, i + 1, (int32_t)(t >> shift), 0);
            acc = mac_check(c, i + 1, (int64_t)m1 * vec[1]);
            acc = mac_check(c, i + 1, acc + (int64_t)m2 * vec[2]);
        } else {
            acc = mac_check(c, i + 1, acc + (int64_t)m0 * vec[0]);
            acc = mac_check(c, i + 1, acc + (int64_t)m1 * vec[1]);
            acc = mac_check(c, i + 1, acc + (int64_t)m2 * vec[2]);
        }
        D[MAC1 + i] = (uint32_t)(int32_t)(acc >> shift);
    }
    macs_to_irs(c, lm);
}

/* lighting helpers --------------------------------------------------- */
static void light_matrix(CPU *c, int v, int sf, int lm) {
    /* IR = sat(L * V) */
    int32_t vec[3] = {VX(c, v), VY(c, v), VZ(c, v)};
    mvmva_core(c, 1, vec, 3, sf, lm);
}

static void color_matrix_bk(CPU *c, int sf, int lm) {
    int32_t vec[3] = {(int16_t)D[IR1], (int16_t)D[IR2], (int16_t)D[IR3]};
    mvmva_core(c, 2, vec, 1, sf, lm);
}

/* MAC = (RGB<<4) * IR, used by NCC/CC */
static void color_mul_ir(CPU *c, int sf) {
    int shift = sf ? 12 : 0;
    uint32_t rgb = D[RGBC];
    int64_t r = (int64_t)((rgb & 0xFF) << 4) * (int16_t)D[IR1];
    int64_t g = (int64_t)(((rgb >> 8) & 0xFF) << 4) * (int16_t)D[IR2];
    int64_t b = (int64_t)(((rgb >> 16) & 0xFF) << 4) * (int16_t)D[IR3];
    set_mac(c, 1, r, shift);
    set_mac(c, 2, g, shift);
    set_mac(c, 3, b, shift);
}

/* depth cue interpolation: MAC = MAC + (FC - MAC) * IR0, applied to the
 * value already in MAC1..3 (expressed in the unshifted 12.12 domain via
 * in[]). */
static void depth_cue(CPU *c, const int64_t in[3], int sf, int lm) {
    int shift = sf ? 12 : 0;
    int32_t fc[3] = {(int32_t)C[RFC], (int32_t)C[GFC], (int32_t)C[BFC]};
    int16_t ir0 = (int16_t)D[IR0];
    int64_t tmp[3];
    for (int i = 0; i < 3; i++) {
        int64_t d = mac_check(c, i + 1, ((int64_t)fc[i] << 12) - in[i]);
        tmp[i] = sat_ir(c, i + 1, (int32_t)(d >> shift), 0);
    }
    for (int i = 0; i < 3; i++) {
        int64_t v = mac_check(c, i + 1, in[i] + tmp[i] * ir0);
        D[MAC1 + i] = (uint32_t)(int32_t)(v >> shift);
    }
    macs_to_irs(c, lm);
}

static void ncd(CPU *c, int v, int sf, int lm) {
    light_matrix(c, v, sf, lm);
    color_matrix_bk(c, sf, lm);
    /* MAC = (RGB<<4)*IR, then depth cue */
    uint32_t rgb = D[RGBC];
    int64_t in[3] = {
        (int64_t)((rgb & 0xFF) << 4) * (int16_t)D[IR1],
        (int64_t)(((rgb >> 8) & 0xFF) << 4) * (int16_t)D[IR2],
        (int64_t)(((rgb >> 16) & 0xFF) << 4) * (int16_t)D[IR3],
    };
    depth_cue(c, in, sf, lm);
    push_color(c);
}

static void ncc(CPU *c, int v, int sf, int lm) {
    light_matrix(c, v, sf, lm);
    color_matrix_bk(c, sf, lm);
    color_mul_ir(c, sf);
    macs_to_irs(c, lm);
    push_color(c);
}

static void nc(CPU *c, int v, int sf, int lm) {
    light_matrix(c, v, sf, lm);
    color_matrix_bk(c, sf, lm);
    push_color(c);
}

static void dpc_color(CPU *c, uint32_t rgb, int sf, int lm) {
    int64_t in[3] = {
        (int64_t)(rgb & 0xFF) << 16,
        (int64_t)((rgb >> 8) & 0xFF) << 16,
        (int64_t)((rgb >> 16) & 0xFF) << 16,
    };
    depth_cue(c, in, sf, lm);
    push_color(c);
}

/* ------------------------------------------------------------------ */

void gte_command(CPU *c, uint32_t cmd) {
    if (!unr_ready)
        unr_init();
    int sf = (cmd >> 19) & 1;
    int lm = (cmd >> 10) & 1;
    int shift = sf ? 12 : 0;
    C[FLAG] = 0;
    switch (cmd & 0x3F) {
    case 0x01: /* RTPS */
        rtp(c, 0, sf, lm, 1);
        break;
    case 0x30: /* RTPT */
        rtp(c, 0, sf, lm, 0);
        rtp(c, 1, sf, lm, 0);
        rtp(c, 2, sf, lm, 1);
        break;
    case 0x06: { /* NCLIP */
        int64_t x0 = lo16(D[SXY0]), y0 = hi16(D[SXY0]);
        int64_t x1 = lo16(D[SXY1]), y1 = hi16(D[SXY1]);
        int64_t x2 = lo16(D[SXY2]), y2 = hi16(D[SXY2]);
        set_mac0(c, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x0C: { /* OP (outer product) */
        int64_t d1 = M(c, 0, 0, 0), d2 = M(c, 0, 1, 1), d3 = M(c, 0, 2, 2);
        int64_t i1 = (int16_t)D[IR1], i2 = (int16_t)D[IR2], i3 = (int16_t)D[IR3];
        set_mac(c, 1, mac_check(c, 1, i3 * d2) - i2 * d3, shift);
        set_mac(c, 2, mac_check(c, 2, i1 * d3) - i3 * d1, shift);
        set_mac(c, 3, mac_check(c, 3, i2 * d1) - i1 * d2, shift);
        macs_to_irs(c, lm);
        break;
    }
    case 0x10: /* DPCS */
        dpc_color(c, D[RGBC], sf, lm);
        break;
    case 0x2A: /* DPCT */
        for (int i = 0; i < 3; i++)
            dpc_color(c, D[RGB0], sf, lm);
        break;
    case 0x11: { /* INTPL */
        int64_t in[3] = {(int64_t)(int16_t)D[IR1] << 12,
                         (int64_t)(int16_t)D[IR2] << 12,
                         (int64_t)(int16_t)D[IR3] << 12};
        depth_cue(c, in, sf, lm);
        push_color(c);
        break;
    }
    case 0x12: { /* MVMVA */
        int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
        int32_t vec[3];
        if (v == 3) {
            vec[0] = (int16_t)D[IR1];
            vec[1] = (int16_t)D[IR2];
            vec[2] = (int16_t)D[IR3];
        } else {
            vec[0] = VX(c, v);
            vec[1] = VY(c, v);
            vec[2] = VZ(c, v);
        }
        mvmva_core(c, mx, vec, cv, sf, lm);
        break;
    }
    case 0x13: ncd(c, 0, sf, lm); break;                        /* NCDS */
    case 0x16: for (int v = 0; v < 3; v++) ncd(c, v, sf, lm); break; /* NCDT */
    case 0x1B: ncc(c, 0, sf, lm); break;                        /* NCCS */
    case 0x3F: for (int v = 0; v < 3; v++) ncc(c, v, sf, lm); break; /* NCCT */
    case 0x1E: nc(c, 0, sf, lm); break;                         /* NCS */
    case 0x20: for (int v = 0; v < 3; v++) nc(c, v, sf, lm); break;  /* NCT */
    case 0x14: { /* CDP */
        color_matrix_bk(c, sf, lm);
        uint32_t rgb = D[RGBC];
        int64_t in[3] = {
            (int64_t)((rgb & 0xFF) << 4) * (int16_t)D[IR1],
            (int64_t)(((rgb >> 8) & 0xFF) << 4) * (int16_t)D[IR2],
            (int64_t)(((rgb >> 16) & 0xFF) << 4) * (int16_t)D[IR3],
        };
        depth_cue(c, in, sf, lm);
        push_color(c);
        break;
    }
    case 0x1C: /* CC */
        color_matrix_bk(c, sf, lm);
        color_mul_ir(c, sf);
        macs_to_irs(c, lm);
        push_color(c);
        break;
    case 0x28: { /* SQR */
        int64_t i1 = (int16_t)D[IR1], i2 = (int16_t)D[IR2], i3 = (int16_t)D[IR3];
        set_mac(c, 1, i1 * i1, shift);
        set_mac(c, 2, i2 * i2, shift);
        set_mac(c, 3, i3 * i3, shift);
        macs_to_irs(c, lm);
        break;
    }
    case 0x29: { /* DCPL */
        uint32_t rgb = D[RGBC];
        int64_t in[3] = {
            (int64_t)((rgb & 0xFF) << 4) * (int16_t)D[IR1],
            (int64_t)(((rgb >> 8) & 0xFF) << 4) * (int16_t)D[IR2],
            (int64_t)(((rgb >> 16) & 0xFF) << 4) * (int16_t)D[IR3],
        };
        depth_cue(c, in, sf, lm);
        push_color(c);
        break;
    }
    case 0x2D: { /* AVSZ3 */
        int64_t s = (int64_t)(int16_t)C[ZSF3] *
                    ((int64_t)(D[SZ1] & 0xFFFF) + (D[SZ2] & 0xFFFF) + (D[SZ3] & 0xFFFF));
        set_mac0(c, s);
        int64_t o = s >> 12;
        if (o < 0) { set_flag(c, 1u << 18); o = 0; }
        else if (o > 0xFFFF) { set_flag(c, 1u << 18); o = 0xFFFF; }
        D[OTZ] = (uint32_t)o;
        break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t s = (int64_t)(int16_t)C[ZSF4] *
                    ((int64_t)(D[SZ0] & 0xFFFF) + (D[SZ1] & 0xFFFF) +
                     (D[SZ2] & 0xFFFF) + (D[SZ3] & 0xFFFF));
        set_mac0(c, s);
        int64_t o = s >> 12;
        if (o < 0) { set_flag(c, 1u << 18); o = 0; }
        else if (o > 0xFFFF) { set_flag(c, 1u << 18); o = 0xFFFF; }
        D[OTZ] = (uint32_t)o;
        break;
    }
    case 0x3D: { /* GPF */
        int64_t ir0 = (int16_t)D[IR0];
        set_mac(c, 1, ir0 * (int16_t)D[IR1], shift);
        set_mac(c, 2, ir0 * (int16_t)D[IR2], shift);
        set_mac(c, 3, ir0 * (int16_t)D[IR3], shift);
        macs_to_irs(c, lm);
        push_color(c);
        break;
    }
    case 0x3E: { /* GPL */
        int64_t ir0 = (int16_t)D[IR0];
        int64_t m1 = (int64_t)(int32_t)D[MAC1] << shift;
        int64_t m2 = (int64_t)(int32_t)D[MAC2] << shift;
        int64_t m3 = (int64_t)(int32_t)D[MAC3] << shift;
        set_mac(c, 1, mac_check(c, 1, m1) + ir0 * (int16_t)D[IR1], shift);
        set_mac(c, 2, mac_check(c, 2, m2) + ir0 * (int16_t)D[IR2], shift);
        set_mac(c, 3, mac_check(c, 3, m3) + ir0 * (int16_t)D[IR3], shift);
        macs_to_irs(c, lm);
        push_color(c);
        break;
    }
    default:
        LOGW("gte", "unknown GTE command %07X", cmd);
        break;
    }
    if (C[FLAG] & 0x7F87E000u)
        C[FLAG] |= 0x80000000u;
}

/* ------------------------------------------------------------------ */
/* register access                                                     */

static uint32_t orgb(CPU *c) {
    int32_t r = (int16_t)D[IR1] >> 7, g = (int16_t)D[IR2] >> 7,
            b = (int16_t)D[IR3] >> 7;
    r = r < 0 ? 0 : (r > 0x1F ? 0x1F : r);
    g = g < 0 ? 0 : (g > 0x1F ? 0x1F : g);
    b = b < 0 ? 0 : (b > 0x1F ? 0x1F : b);
    return (uint32_t)(r | (g << 5) | (b << 10));
}

uint32_t gte_read_data(CPU *c, int reg) {
    switch (reg) {
    case VZ0: case VZ1: case VZ2: case IR0: case IR1: case IR2: case IR3:
        return (uint32_t)(int32_t)(int16_t)D[reg];
    case OTZ: case SZ0: case SZ1: case SZ2: case SZ3:
        return D[reg] & 0xFFFF;
    case SXYP:
        return D[SXY2];
    case IRGB: case ORGB:
        return orgb(c);
    default:
        return D[reg];
    }
}

void gte_write_data(CPU *c, int reg, uint32_t v) {
    switch (reg) {
    case SXYP:
        D[SXY0] = D[SXY1];
        D[SXY1] = D[SXY2];
        D[SXY2] = v;
        break;
    case IRGB:
        D[IRGB] = v & 0x7FFF;
        D[IR1] = (v & 0x1F) << 7;
        D[IR2] = ((v >> 5) & 0x1F) << 7;
        D[IR3] = ((v >> 10) & 0x1F) << 7;
        break;
    case ORGB:
    case LZCR:
        break;
    case LZCS: {
        D[LZCS] = v;
        uint32_t x = (v & 0x80000000u) ? ~v : v;
        D[LZCR] = x ? (uint32_t)__builtin_clz(x) : 32;
        break;
    }
    default:
        D[reg] = v;
        break;
    }
}

uint32_t gte_read_ctrl(CPU *c, int reg) {
    switch (reg) {
    case RT33: case L33: case LB3: case H: case DQA: case ZSF3: case ZSF4:
        /* 16-bit registers read back sign-extended (including H) */
        return (uint32_t)(int32_t)(int16_t)C[reg];
    default:
        return C[reg];
    }
}

void gte_write_ctrl(CPU *c, int reg, uint32_t v) {
    switch (reg) {
    case RT33: case L33: case LB3: case H: case DQA: case ZSF3: case ZSF4:
        C[reg] = v & 0xFFFF;
        break;
    case FLAG:
        C[FLAG] = v & 0x7FFFF000u;
        if (C[FLAG] & 0x7F87E000u)
            C[FLAG] |= 0x80000000u;
        break;
    default:
        C[reg] = v;
        break;
    }
}
