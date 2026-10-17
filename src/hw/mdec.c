/*
 * MDEC: macroblock decoder (run-length DCT coefficients -> IDCT -> YUV to
 * RGB), used for FMV (STR) playback.
 */
#include <stdlib.h>

#include "psx.h"

static const uint8_t k_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static struct {
    uint8_t qt_y[64], qt_c[64];
    int16_t scale[64];
    uint32_t cmd;
    int params_left;          /* words still expected for the command */
    uint16_t *in;             /* halfwords of the current decode command */
    size_t in_len, in_cap;
    int qt_mode;              /* 0 none, 1 luma only, 2 luma+chroma */
    int qt_pos;
    int scale_pos;
    uint32_t *out;
    size_t out_len, out_pos, out_cap;
    bool enable_in, enable_out;
} M;

void mdec_init(void) {
    free(M.in);
    free(M.out);
    memset(&M, 0, sizeof M);
}

static inline int s10(int v) { return (v & 0x200) ? v - 0x400 : v; }

/* returns number of halfwords consumed, or -1 if input incomplete */
static int rl_decode(const uint16_t *src, size_t avail, int16_t blk[64],
                     const uint8_t *qt) {
    size_t i = 0;
    memset(blk, 0, 64 * sizeof(int16_t));
    while (i < avail && src[i] == 0xFE00)
        i++;
    if (i >= avail)
        return -1;
    uint16_t n = src[i++];
    int k = 0;
    int q_scale = (n >> 10) & 0x3F;
    int val = s10(n & 0x3FF) * qt[0];
    for (;;) {
        if (q_scale == 0)
            val = s10(n & 0x3FF) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        if (q_scale > 0)
            blk[k_zigzag[k]] = (int16_t)val;
        else
            blk[k] = (int16_t)val;
        if (i >= avail)
            return -1;
        n = src[i++];
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63)
            break;
        val = (s10(n & 0x3FF) * qt[k] * q_scale + 4) / 8;
    }
    return (int)i;
}

static void idct(int16_t blk[64]) {
    int32_t tmp[64];
    int16_t *src = blk;
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int32_t sum = 0;
            for (int z = 0; z < 8; z++)
                sum += src[y + z * 8] * (M.scale[x + z * 8] / 8);
            tmp[x + y * 8] = (sum + 0xFFF) >> 13;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int32_t sum = 0;
            for (int z = 0; z < 8; z++)
                sum += tmp[y + z * 8] * (M.scale[x + z * 8] / 8);
            int32_t v = (sum + 0xFFF) >> 13;
            blk[x + y * 8] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
        }
}

static void out_reserve(size_t words) {
    if (M.out_len + words > M.out_cap) {
        M.out_cap = (M.out_len + words) * 2;
        M.out = realloc(M.out, M.out_cap * 4);
    }
}

static inline int clamp_s8(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }

static void emit_color_mb(int16_t cr[64], int16_t cb[64], int16_t y[4][64]) {
    int depth = (M.cmd >> 27) & 3;
    bool is_signed = (M.cmd >> 26) & 1;
    bool bit15 = (M.cmd >> 25) & 1;
    uint8_t rgb[16][16][3];
    for (int b = 0; b < 4; b++) {
        int xx = (b & 1) * 8, yy = (b >> 1) * 8;
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++) {
                int ci = (xx + i) / 2 + ((yy + j) / 2) * 8;
                int R = cr[ci], B = cb[ci];
                int G = (-0x581 * B - 0xB6C * R) >> 12; /* -0.3437 B -0.7143 R */
                R = (0x166E * R) >> 12;                 /* 1.402 */
                B = (0x1C5A * B) >> 12;                 /* 1.772 */
                int Y = y[b][i + j * 8];
                int r = clamp_s8(Y + R), g = clamp_s8(Y + G), bb = clamp_s8(Y + B);
                if (!is_signed) {
                    r ^= 0x80;
                    g ^= 0x80;
                    bb ^= 0x80;
                }
                rgb[yy + j][xx + i][0] = (uint8_t)r;
                rgb[yy + j][xx + i][1] = (uint8_t)g;
                rgb[yy + j][xx + i][2] = (uint8_t)bb;
            }
    }
    if (depth == 2) { /* 24-bit: 16x16x3 = 768 bytes = 192 words */
        out_reserve(192);
        uint8_t *dst = (uint8_t *)(M.out + M.out_len);
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++)
                for (int k = 0; k < 3; k++)
                    *dst++ = rgb[j][i][k];
        M.out_len += 192;
    } else { /* 15-bit: 256 pixels * 2 bytes = 128 words */
        out_reserve(128);
        uint16_t *dst = (uint16_t *)(M.out + M.out_len);
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++) {
                uint16_t px = (uint16_t)((rgb[j][i][0] >> 3) |
                                         ((rgb[j][i][1] >> 3) << 5) |
                                         ((rgb[j][i][2] >> 3) << 10));
                if (bit15)
                    px |= 0x8000;
                *dst++ = px;
            }
        M.out_len += 128;
    }
}

static void emit_mono_block(int16_t y[64]) {
    int depth = (M.cmd >> 27) & 3;
    bool is_signed = (M.cmd >> 26) & 1;
    uint8_t px[64];
    for (int i = 0; i < 64; i++) {
        int v = clamp_s8(y[i]);
        if (!is_signed)
            v ^= 0x80;
        px[i] = (uint8_t)v;
    }
    if (depth == 1) { /* 8-bit */
        out_reserve(16);
        memcpy(M.out + M.out_len, px, 64);
        M.out_len += 16;
    } else { /* 4-bit */
        out_reserve(8);
        uint8_t *dst = (uint8_t *)(M.out + M.out_len);
        for (int i = 0; i < 32; i++)
            dst[i] = (uint8_t)((px[i * 2] >> 4) | (px[i * 2 + 1] & 0xF0));
        M.out_len += 8;
    }
}

static void decode_all(void) {
    int depth = (M.cmd >> 27) & 3;
    size_t pos = 0;
    int16_t cr[64], cb[64], y[4][64], mono[64];
    while (pos < M.in_len) {
        int used;
        if (depth >= 2) {
            if ((used = rl_decode(M.in + pos, M.in_len - pos, cr, M.qt_c)) < 0) break;
            pos += (size_t)used;
            if ((used = rl_decode(M.in + pos, M.in_len - pos, cb, M.qt_c)) < 0) break;
            pos += (size_t)used;
            bool ok = true;
            for (int b = 0; b < 4 && ok; b++) {
                if ((used = rl_decode(M.in + pos, M.in_len - pos, y[b], M.qt_y)) < 0)
                    ok = false;
                else
                    pos += (size_t)used;
            }
            if (!ok)
                break;
            idct(cr);
            idct(cb);
            for (int b = 0; b < 4; b++)
                idct(y[b]);
            emit_color_mb(cr, cb, y);
        } else {
            if ((used = rl_decode(M.in + pos, M.in_len - pos, mono, M.qt_y)) < 0)
                break;
            pos += (size_t)used;
            idct(mono);
            emit_mono_block(mono);
        }
    }
}

static void feed_word(uint32_t v) {
    if (M.params_left > 0) {
        uint32_t op = M.cmd >> 29;
        if (op == 1) {
            if (M.in_len + 2 > M.in_cap) {
                M.in_cap = (M.in_len + 2) * 2;
                M.in = realloc(M.in, M.in_cap * 2);
            }
            M.in[M.in_len++] = (uint16_t)v;
            M.in[M.in_len++] = (uint16_t)(v >> 16);
        } else if (op == 2) {
            for (int k = 0; k < 4; k++) {
                uint8_t b = (uint8_t)(v >> (8 * k));
                if (M.qt_pos < 64)
                    M.qt_y[M.qt_pos] = b;
                else
                    M.qt_c[M.qt_pos - 64] = b;
                M.qt_pos++;
            }
        } else if (op == 3) {
            M.scale[M.scale_pos++ & 63] = (int16_t)v;
            M.scale[M.scale_pos++ & 63] = (int16_t)(v >> 16);
        }
        if (--M.params_left == 0 && op == 1) {
            decode_all();
            M.in_len = 0;
        }
        return;
    }
    M.cmd = v;
    uint32_t op = v >> 29;
    switch (op) {
    case 1:
        M.params_left = (int)(v & 0xFFFF);
        M.in_len = 0;
        /* Output is a FIFO: a new decode queued while the previous one is
         * still being read appends after it (the hardware would stall the
         * input DMA until the output drained; the data order is the same). */
        if (M.out_pos > 0) {
            memmove(M.out, M.out + M.out_pos, (M.out_len - M.out_pos) * 4);
            M.out_len -= M.out_pos;
            M.out_pos = 0;
        }
        break;
    case 2:
        M.params_left = (v & 1) ? 32 : 16;
        M.qt_pos = 0;
        break;
    case 3:
        M.params_left = 32;
        M.scale_pos = 0;
        break;
    default:
        M.params_left = 0;
        break;
    }
}

uint32_t mdec_read(uint32_t addr) {
    if (addr != 0x1F801820) LOGD("mdec", "status read params_left=%d out=%zu/%zu", M.params_left, M.out_pos, M.out_len);
    if (addr == 0x1F801820) {
        if (M.out_pos < M.out_len)
            return M.out[M.out_pos++];
        return 0;
    }
    uint32_t st = 0;
    bool out_empty = M.out_pos >= M.out_len;
    st |= (uint32_t)out_empty << 31;
    st |= (uint32_t)(M.params_left > 0) << 29;
    st |= (uint32_t)(M.enable_in && M.params_left > 0) << 28;
    st |= (uint32_t)(M.enable_out && !out_empty) << 27;
    st |= ((M.cmd >> 25) & 0xF) << 23;
    st |= (uint32_t)((M.params_left - 1) & 0xFFFF);
    return st;
}

void mdec_write(uint32_t addr, uint32_t v) {
    LOGD("mdec", "write %08X = %08X (params_left=%d)", addr, v, M.params_left);
    if (addr == 0x1F801820) {
        feed_word(v);
        return;
    }
    if (v & 0x80000000u) {
        M.params_left = 0;
        M.in_len = M.out_len = M.out_pos = 0;
        M.cmd = 0;
    }
    M.enable_in = (v >> 30) & 1;
    M.enable_out = (v >> 29) & 1;
}

void mdec_dma_in(uint32_t addr, uint32_t words) {
    LOGD("mdec", "dma in %08X %u words, first=%08X params_left=%d", addr, words, mem_r32(addr), M.params_left);
    for (uint32_t i = 0; i < words; i++)
        feed_word(mem_r32(addr + i * 4));
}

void mdec_dma_out(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        if (M.out_pos < M.out_len)
            w = M.out[M.out_pos++];
        mem_w32(addr + i * 4, w);
    }
}
