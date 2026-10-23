/*
 * GPU command processor: GP0 (drawing / VRAM transfer) and GP1 (display
 * control) ports, GPUSTAT, DMA interfaces and video timing (vblank).
 * Rasterization is delegated to a GpuBackend.
 */
#include <stdlib.h>

#include "gpu.h"
#include "pgxp.h"
#include "psx.h"
#include "savestate.h"
#include "widescreen.h"

uint16_t g_vram[VRAM_W * VRAM_H];
const GpuBackend *g_gpu_backend = &g_gpu_sw;
uint64_t g_gpu_frame_counter;
uint64_t g_gpu_flip_counter;  /* display buffer changes = game frames */

static GpuState S;
extern int32_t g_ot_slot; /* OT slot of the primitive being decoded */

/* display state */
static struct {
    bool disabled;
    int dma_dir;
    int disp_x, disp_y;
    int h_start, h_end, v_start, v_end;
    uint32_t mode;     /* GP1(08) bits */
    bool irq;
    bool field;        /* interlace field / odd line */
} D;

/* GP0 command FIFO (with the RAM address each word came from, 0 = CPU
 * write, used to find PGXP precision data for vertices) */
static uint32_t g_fifo[16];
static uint32_t g_fifo_addr[16];
static uint32_t g_gp0_src;
static int g_fifo_n;
static int g_fifo_need;
static bool g_polyline;

/* CPU->VRAM / VRAM->CPU transfer state */
static struct {
    bool active;
    int x, y, w, h;
    int cx, cy;
} g_wr, g_rd;

/* ------------------------------------------------------------------ */

static void gpu_reset_cmd(void) {
    g_fifo_n = 0;
    g_fifo_need = 0;
    g_polyline = false;
    g_wr.active = false;
    g_rd.active = false;
}

void gpu_init(void) {
    memset(&S, 0, sizeof S);
    memset(&D, 0, sizeof D);
    memset(g_vram, 0, sizeof g_vram);
    D.disabled = true;
    D.h_start = 0x200;
    D.h_end = 0xC00;
    D.v_start = 0x10;
    D.v_end = 0x100;
    S.area_x2 = 1023;
    S.area_y2 = 511;
    gpu_reset_cmd();
}

uint16_t *gpu_vram(void) { return g_vram; }

uint32_t gpu_read_stat(void) {
    uint32_t s = 0;
    s |= S.tpage & 0x1FF;
    s |= (uint32_t)S.dither << 9;
    s |= (uint32_t)S.draw_to_display << 10;
    s |= (uint32_t)S.set_mask << 11;
    s |= (uint32_t)S.check_mask << 12;
    s |= (uint32_t)S.tex_disable << 15;
    /* display mode bits */
    s |= ((D.mode >> 6) & 1) << 16;
    s |= (D.mode & 3) << 17;
    s |= ((D.mode >> 2) & 1) << 19;
    s |= ((D.mode >> 3) & 1) << 20;
    s |= ((D.mode >> 4) & 1) << 21;
    s |= ((D.mode >> 5) & 1) << 22;
    s |= (uint32_t)D.disabled << 23;
    s |= (uint32_t)D.irq << 24;
    bool interlace = (D.mode >> 5) & 1;
    if (interlace)
        s |= (uint32_t)D.field << 13;
    /* always ready */
    uint32_t ready_cmd = 1, ready_vram = g_rd.active ? 1 : 0, ready_dma = 1;
    s |= ready_cmd << 26;
    s |= ready_vram << 27;
    s |= ready_dma << 28;
    s |= (uint32_t)D.dma_dir << 29;
    switch (D.dma_dir) {
    case 1: s |= 1u << 25; break;
    case 2: s |= ready_dma << 25; break;
    case 3: s |= ready_vram << 25; break;
    default: break;
    }
    /* odd/even line toggles each scanline in progressive mode; use the
     * scanline parity derived from the cycle counter */
    if (!interlace) {
        int64_t line = (now_cycles() / PSX_HBLANK_CYCLES);
        s |= (uint32_t)(line & 1) << 31;
    } else {
        s |= (uint32_t)D.field << 31;
    }
    return s;
}

uint32_t gpu_read_data(void) {
    if (!g_rd.active)
        return 0;
    uint32_t out = 0;
    for (int k = 0; k < 2; k++) {
        int x = (g_rd.x + g_rd.cx) & 1023;
        int y = (g_rd.y + g_rd.cy) & 511;
        out |= (uint32_t)g_vram[y * VRAM_W + x] << (16 * k);
        if (++g_rd.cx >= g_rd.w) {
            g_rd.cx = 0;
            if (++g_rd.cy >= g_rd.h) {
                g_rd.active = false;
                break;
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* primitive decoding                                                  */

static void set_tpage(uint16_t tp) {
    S.tpage = tp & 0x9FF;
    S.tex_disable = (tp >> 11) & 1;
}

static void unpack_xy(uint32_t w, GVtx *v) {
    v->x = sext11((int32_t)(w & 0x7FF)) + S.off_x;
    v->y = sext11((int32_t)((w >> 16) & 0x7FF)) + S.off_y;
}

static void unpack_color(uint32_t w, GVtx *v) {
    v->r = (uint8_t)w;
    v->g = (uint8_t)(w >> 8);
    v->b = (uint8_t)(w >> 16);
}

/* Widescreen: decide whether a primitive is a 2D overlay. The game puts its
 * HUD, text, menus and 2D backgrounds in the nearest ordering-table slots
 * (or draws them outside an OT); 3D geometry, including near-camera
 * particles, starts much deeper. A primitive in an overlay slot is still
 * treated as 3D when every vertex is an exact GTE projection.
 * xs/ys are raw packet coordinates (without the drawing offset). */
#define WS_OVERLAY_SLOTS 16

static uint32_t ws_classify(const int32_t *xs, const int32_t *ys, int n) {
    if (!g_ws_enabled)
        return 0;
    bool overlay = g_ot_slot < WS_OVERLAY_SLOTS;
    if (overlay) {
        int m = 0;
        for (int i = 0; i < n; i++)
            m += ws_projected_exact(xs[i], ys[i]);
        overlay = m != n;
    }
    if (!overlay) {
        g_ws_3d_count++;
        return 0;
    }
    if (!g_ws_wide_display)
        return 0; /* 2D-only screen shown at 4:3: draw it untouched */
    S.ws_anchor = (float)(S.area_x1 + S.area_x2 + 1) * 0.5f;
    return PRIM_WS_2D;
}

static int poly_words(uint32_t cmd) {
    bool shaded = (cmd >> 4) & 1, quad = (cmd >> 3) & 1, tex = (cmd >> 2) & 1;
    int nv = quad ? 4 : 3;
    int per = 1 + (tex ? 1 : 0) + (shaded ? 1 : 0);
    return nv * per + (shaded ? 0 : 1);
}

/* debug: OT_ONLY=lo:hi draws only primitives from that OT slot range */
static bool ot_filtered(void) {
    static int init, lo, hi;
    if (!init) {
        const char *e = getenv("OT_ONLY");
        init = 1;
        lo = 0, hi = -1;
        if (e)
            sscanf(e, "%d:%d", &lo, &hi);
    }
    return hi >= lo && (g_ot_slot < lo || g_ot_slot > hi);
}

static void draw_polygon(const uint32_t *w) {
    if (ot_filtered())
        return;
    uint32_t cmd = w[0] >> 24;
    bool shaded = (cmd >> 4) & 1, quad = (cmd >> 3) & 1, tex = (cmd >> 2) & 1;
    int nv = quad ? 4 : 3;
    GVtx v[4];
    uint16_t clut = 0, tpage = S.tpage;
    int i = 0;
    for (int k = 0; k < nv; k++) {
        if (k == 0 || shaded)
            unpack_color(w[i++], &v[k]);
        else
            v[k].r = v[0].r, v[k].g = v[0].g, v[k].b = v[0].b;
        v[k].precise = v[k].tracked = false;
        if (g_pgxp) {
            PgxpVtx p;
            if (pgxp_lookup(g_fifo_addr[i], w[i], &p)) {
                float ox = (float)S.off_x, oy = (float)S.off_y, sx, sy;
                v[k].tracked = true;
                v[k].hx = p.x + ox * p.w;
                v[k].hy = p.y + oy * p.w;
                v[k].hw = p.w;
                if (pgxp_screen(&p, &sx, &sy)) {
                    v[k].precise = true;
                    v[k].fx = sx + ox;
                    v[k].fy = sy + oy;
                    v[k].fz = p.w;
                }
            }
        }
        unpack_xy(w[i++], &v[k]);
        if (tex) {
            uint32_t uv = w[i++];
            v[k].u = (uint8_t)uv;
            v[k].v = (uint8_t)(uv >> 8);
            if (k == 0)
                clut = (uint16_t)(uv >> 16);
            else if (k == 1)
                tpage = (uint16_t)(uv >> 16);
        } else {
            v[k].u = v[k].v = 0;
        }
    }
    if (tex)
        set_tpage((uint16_t)((S.tpage & ~0x9FFu) | (tpage & 0x9FF)));
    uint32_t flags = (shaded ? PRIM_SHADED : 0) | (tex ? PRIM_TEXTURED : 0) |
                     ((cmd & 2) ? PRIM_SEMI : 0) | ((cmd & 1) ? PRIM_RAW : 0);
    if (tex && S.tex_disable)
        flags &= ~(uint32_t)(PRIM_TEXTURED | PRIM_RAW);
    if (g_ws_enabled) {
        bool any_tracked = false;
        for (int k = 0; k < nv; k++)
            any_tracked |= v[k].tracked;
        if (any_tracked) {
            g_ws_3d_count++; /* tracked GTE projection: certainly 3D */
        } else {
            int32_t xs[4], ys[4];
            for (int k = 0; k < nv; k++)
                xs[k] = v[k].x - S.off_x, ys[k] = v[k].y - S.off_y;
            flags |= ws_classify(xs, ys, nv);
        }
    }
    g_gpu_backend->triangle(&S, v, flags, clut, tpage);
    if (quad) {
        GVtx t[3] = {v[1], v[2], v[3]};
        g_gpu_backend->triangle(&S, t, flags, clut, tpage);
    }
}

static int rect_words(uint32_t cmd) {
    int n = 2;
    if ((cmd >> 2) & 1)
        n++;
    if (((cmd >> 3) & 3) == 0)
        n++;
    return n;
}

static void draw_rect(const uint32_t *w) {
    uint32_t cmd = w[0] >> 24;
    bool tex = (cmd >> 2) & 1;
    int size = (cmd >> 3) & 3;
    GVtx v = {0};
    unpack_color(w[0], &v);
    unpack_xy(w[1], &v);
    int i = 2;
    uint16_t clut = 0;
    v.u = v.v = 0;
    if (tex) {
        uint32_t uv = w[i++];
        v.u = (uint8_t)uv;
        v.v = (uint8_t)(uv >> 8);
        clut = (uint16_t)(uv >> 16);
    }
    int wd, ht;
    switch (size) {
    case 1: wd = ht = 1; break;
    case 2: wd = ht = 8; break;
    case 3: wd = ht = 16; break;
    default:
        wd = (int)(w[i] & 0x3FF);
        ht = (int)((w[i] >> 16) & 0x1FF);
        break;
    }
    uint32_t flags = (tex ? PRIM_TEXTURED : 0) | ((cmd & 2) ? PRIM_SEMI : 0) |
                     ((cmd & 1) ? PRIM_RAW : 0);
    if (tex && S.tex_disable)
        flags &= ~(uint32_t)(PRIM_TEXTURED | PRIM_RAW);
    if (g_ws_enabled) {
        int32_t xs[4] = {v.x - S.off_x, v.x - S.off_x + wd, v.x - S.off_x,
                         v.x - S.off_x + wd};
        int32_t ys[4] = {v.y - S.off_y, v.y - S.off_y, v.y - S.off_y + ht,
                         v.y - S.off_y + ht};
        flags |= ws_classify(xs, ys, 4);
    }
    if (wd && ht)
        g_gpu_backend->rect(&S, &v, wd, ht, flags, clut);
}

/* line state for polylines */
static GVtx g_line_last;
static uint32_t g_line_flags;
static bool g_line_shaded;
static bool g_line_have_color;
static GVtx g_line_pending_color;

static void draw_line_cmd(const uint32_t *w) {
    uint32_t cmd = w[0] >> 24;
    bool shaded = (cmd >> 4) & 1;
    GVtx a = {0}, b = {0};
    unpack_color(w[0], &a);
    unpack_xy(w[1], &a);
    if (shaded) {
        unpack_color(w[2], &b);
        unpack_xy(w[3], &b);
    } else {
        b.r = a.r, b.g = a.g, b.b = a.b;
        unpack_xy(w[2], &b);
    }
    a.u = a.v = b.u = b.v = 0;
    uint32_t flags = (shaded ? PRIM_SHADED : 0) | ((cmd & 2) ? PRIM_SEMI : 0);
    if (g_ws_enabled) {
        int32_t xs[2] = {a.x - S.off_x, b.x - S.off_x};
        int32_t ys[2] = {a.y - S.off_y, b.y - S.off_y};
        flags |= ws_classify(xs, ys, 2);
    }
    g_gpu_backend->line(&S, &a, &b, flags);
    g_line_last = b;
    g_line_flags = flags;
    g_line_shaded = shaded;
}

/* ------------------------------------------------------------------ */
/* VRAM transfers                                                      */

static void vram_fill(const uint32_t *w) {
    uint32_t c = w[0];
    uint16_t col = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) |
                              (((c >> 19) & 0x1F) << 10));
    int x = (int)(w[1] & 0x3F0);
    int y = (int)((w[1] >> 16) & 0x1FF);
    int wd = (int)(((w[2] & 0x3FF) + 0xF) & ~0xFu);
    int ht = (int)((w[2] >> 16) & 0x1FF);
    if (wd && ht)
        g_gpu_backend->fill(x, y, wd, ht, col);
}

static void vram_copy(const uint32_t *w) {
    int sx = (int)(w[1] & 0x3FF), sy = (int)((w[1] >> 16) & 0x1FF);
    int dx = (int)(w[2] & 0x3FF), dy = (int)((w[2] >> 16) & 0x1FF);
    int wd = (int)(((w[3] & 0xFFFF) - 1) & 0x3FF) + 1;
    int ht = (int)((((w[3] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    g_gpu_backend->vram_copy(&S, sx, sy, dx, dy, wd, ht);
}

/* CPU->VRAM data is gathered into a staging buffer and handed to the
 * backend in one piece */
static uint16_t *g_wr_buf;
static size_t g_wr_cap, g_wr_count;

static void vram_write_begin(const uint32_t *w) {
    g_wr.x = (int)(w[1] & 0x3FF);
    g_wr.y = (int)((w[1] >> 16) & 0x1FF);
    g_wr.w = (int)(((w[2] & 0xFFFF) - 1) & 0x3FF) + 1;
    g_wr.h = (int)((((w[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    g_wr.cx = g_wr.cy = 0;
    g_wr.active = true;
    size_t n = (size_t)g_wr.w * (size_t)g_wr.h;
    if (n > g_wr_cap) {
        g_wr_cap = n;
        g_wr_buf = realloc(g_wr_buf, n * 2);
    }
    g_wr_count = 0;
}

static void vram_write_word(uint32_t v) {
    size_t total = (size_t)g_wr.w * (size_t)g_wr.h;
    for (int k = 0; k < 2 && g_wr_count < total; k++)
        g_wr_buf[g_wr_count++] = (uint16_t)(v >> (16 * k));
    if (g_wr_count >= total) {
        g_wr.active = false;
        g_gpu_backend->vram_write(&S, g_wr.x, g_wr.y, g_wr.w, g_wr.h,
                                  g_wr_buf);
    }
}

static void vram_read_begin(const uint32_t *w) {
    g_rd.x = (int)(w[1] & 0x3FF);
    g_rd.y = (int)((w[1] >> 16) & 0x1FF);
    g_rd.w = (int)(((w[2] & 0xFFFF) - 1) & 0x3FF) + 1;
    g_rd.h = (int)((((w[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    g_rd.cx = g_rd.cy = 0;
    g_rd.active = true;
    if (g_gpu_backend->vram_sync)
        g_gpu_backend->vram_sync(g_rd.x, g_rd.y, g_rd.w, g_rd.h);
}

/* ------------------------------------------------------------------ */
/* GP0                                                                 */

static int cmd_length(uint32_t w) {
    uint32_t cmd = w >> 24;
    switch (cmd >> 5) {
    case 1: return poly_words(cmd);
    case 2: return ((cmd >> 4) & 1) ? 4 : 3; /* first segment */
    case 3: return rect_words(cmd);
    case 4: return 4;
    case 5: return 3;
    case 6: return 3;
    default:
        if (cmd == 0x02)
            return 3;
        return 1;
    }
}

static void exec_env(uint32_t w) {
    uint32_t cmd = w >> 24;
    switch (cmd) {
    case 0xE1:
        set_tpage((uint16_t)((w & 0x1FF) | ((w >> 11) & 1) << 11));
        S.dither = (w >> 9) & 1;
        S.draw_to_display = (w >> 10) & 1;
        S.rect_flip_x = (w >> 12) & 1;
        S.rect_flip_y = (w >> 13) & 1;
        break;
    case 0xE2:
        S.tw_mask_x = w & 0x1F;
        S.tw_mask_y = (w >> 5) & 0x1F;
        S.tw_off_x = (w >> 10) & 0x1F;
        S.tw_off_y = (w >> 15) & 0x1F;
        break;
    case 0xE3:
        S.area_x1 = (int)(w & 0x3FF);
        S.area_y1 = (int)((w >> 10) & 0x1FF);
        break;
    case 0xE4:
        S.area_x2 = (int)(w & 0x3FF);
        S.area_y2 = (int)((w >> 10) & 0x1FF);
        break;
    case 0xE5:
        S.off_x = sext11((int32_t)(w & 0x7FF));
        S.off_y = sext11((int32_t)((w >> 11) & 0x7FF));
        break;
    case 0xE6:
        S.set_mask = w & 1;
        S.check_mask = (w >> 1) & 1;
        break;
    default:
        break;
    }
}

static void exec_command(const uint32_t *w) {
    uint32_t cmd = w[0] >> 24;
    switch (cmd >> 5) {
    case 0:
        if (cmd == 0x02)
            vram_fill(w);
        else if (cmd == 0x1F) {
            D.irq = true;
            irq_raise(IRQ_GPU);
        }
        break;
    case 1: draw_polygon(w); break;
    case 2: draw_line_cmd(w); break;
    case 3: draw_rect(w); break;
    case 4: vram_copy(w); break;
    case 5: vram_write_begin(w); break;
    case 6: vram_read_begin(w); break;
    case 7: exec_env(w[0]); break;
    }
}

void gpu_write_gp0(uint32_t v) {
    if (g_wr.active) {
        vram_write_word(v);
        return;
    }
    if (g_polyline) {
        if ((v & 0xF000F000u) == 0x50005000u) {
            g_polyline = false;
            return;
        }
        if (g_line_shaded && !g_line_have_color) {
            unpack_color(v, &g_line_pending_color);
            g_line_have_color = true;
            return;
        }
        GVtx b = {0};
        if (g_line_shaded)
            b = g_line_pending_color;
        else
            b.r = g_line_last.r, b.g = g_line_last.g, b.b = g_line_last.b;
        unpack_xy(v, &b);
        b.u = b.v = 0;
        g_gpu_backend->line(&S, &g_line_last, &b, g_line_flags);
        g_line_last = b;
        g_line_have_color = false;
        return;
    }
    if (g_fifo_n == 0) {
        g_fifo_need = cmd_length(v);
    }
    g_fifo_addr[g_fifo_n] = g_gp0_src;
    g_fifo[g_fifo_n++] = v;
    if (g_fifo_n >= g_fifo_need) {
        uint32_t cmd = g_fifo[0] >> 24;
        exec_command(g_fifo);
        g_fifo_n = 0;
        if ((cmd >> 5) == 2 && (cmd & 0x08)) {
            g_polyline = true;
            g_line_have_color = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* GP1                                                                 */

void gpu_write_gp1(uint32_t v) {
    uint32_t cmd = (v >> 24) & 0x3F;
    switch (cmd) {
    case 0x00:
        gpu_reset_cmd();
        D.irq = false;
        D.disabled = true;
        D.dma_dir = 0;
        D.disp_x = D.disp_y = 0;
        D.mode = 0;
        set_tpage(0);
        S.dither = false;
        S.tw_mask_x = S.tw_mask_y = S.tw_off_x = S.tw_off_y = 0;
        S.area_x1 = S.area_y1 = 0;
        S.area_x2 = S.area_y2 = 0;
        S.off_x = S.off_y = 0;
        S.set_mask = S.check_mask = false;
        break;
    case 0x01:
        gpu_reset_cmd();
        break;
    case 0x02:
        D.irq = false;
        break;
    case 0x03:
        D.disabled = v & 1;
        break;
    case 0x04:
        D.dma_dir = (int)(v & 3);
        break;
    case 0x05: {
        int nx = (int)(v & 0x3FE), ny = (int)((v >> 10) & 0x1FF);
        if (nx != D.disp_x || ny != D.disp_y)
            g_gpu_flip_counter++;
        D.disp_x = nx;
        D.disp_y = ny;
        break;
    }
    case 0x06:
        D.h_start = (int)(v & 0xFFF);
        D.h_end = (int)((v >> 12) & 0xFFF);
        break;
    case 0x07:
        D.v_start = (int)(v & 0x3FF);
        D.v_end = (int)((v >> 10) & 0x3FF);
        break;
    case 0x08:
        D.mode = v & 0xFF;
        break;
    case 0x10:
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C:
    case 0x1D: case 0x1E: case 0x1F:
        /* GPU info: answered through GPUREAD; rarely used */
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* DMA                                                                 */

void gpu_dma_write(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        g_gp0_src = addr + i * 4;
        gpu_write_gp0(mem_r32(addr + i * 4));
    }
    g_gp0_src = 0;
}

void gpu_dma_read(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++)
        mem_w32(addr + i * 4, gpu_read_data());
}

/* ordering-table bookkeeping: the OT arrays are known from OTC DMA clears;
 * while walking a linked list the slot of the last OT entry passed tells
 * where in depth order the following primitives were inserted */
int32_t g_ot_slot = -1;
static uint32_t g_ot_lo[4], g_ot_hi[4];
static int g_ot_n;

void gpu_note_ot(uint32_t lo, uint32_t hi) {
    for (int i = 0; i < g_ot_n; i++)
        if (g_ot_lo[i] == lo && g_ot_hi[i] == hi)
            return;
    if (g_ot_n < 4) {
        g_ot_lo[g_ot_n] = lo;
        g_ot_hi[g_ot_n] = hi;
        g_ot_n++;
    }
}

static int32_t ot_slot_of(uint32_t a) {
    for (int i = 0; i < g_ot_n; i++)
        if (a >= g_ot_lo[i] && a <= g_ot_hi[i])
            return (int32_t)((a - g_ot_lo[i]) >> 2);
    return -2;
}

uint32_t gpu_dma_linked_list(uint32_t addr) {
    uint32_t total = 0;
    int guard = 0;
    addr &= 0x1FFFFC;
    g_ot_slot = -1;
    for (;;) {
        uint32_t hdr = mem_r32(addr);
        uint32_t n = hdr >> 24;
        int32_t sl = ot_slot_of(addr);
        if (sl >= 0)
            g_ot_slot = sl;
        for (uint32_t i = 0; i < n; i++) {
            g_gp0_src = addr + 4 + i * 4;
            gpu_write_gp0(mem_r32(addr + 4 + i * 4));
        }
        g_gp0_src = 0;
        total += n + 1;
        if (hdr & 0x800000u)
            break;
        addr = hdr & 0x1FFFFC;
        if (++guard > 0x200000) {
            LOGE("gpu", "linked list too long (loop?)");
            break;
        }
    }
    g_ot_slot = -1; /* later direct GP0 writes are not part of an OT */
    return total;
}

/* ------------------------------------------------------------------ */
/* display                                                             */

void gpu_display_info(DisplayInfo *d) {
    static const int hres_tab[4] = {256, 320, 512, 640};
    int hres = (D.mode & 0x40) ? 368 : hres_tab[D.mode & 3];
    bool interlace = (D.mode >> 5) & 1;
    bool vres480 = ((D.mode >> 2) & 1) && interlace;
    int lines = D.v_end - D.v_start;
    if (lines <= 0 || lines > 300)
        lines = 240;
    d->x = D.disp_x;
    d->y = D.disp_y;
    d->w = hres;
    d->h = vres480 ? lines * 2 : lines;
    d->rgb24 = (D.mode >> 4) & 1;
    d->enabled = !D.disabled;
    d->interlaced = interlace;
    d->pal = (D.mode >> 3) & 1;
    d->hres = hres;
}

/* save states: VRAM (read back from the renderer first), the drawing and
 * display state and any half-received command or transfer */
void gpu_state(StateIO *s) {
    state_section(s, "GPU ");
    if (!s->loading && g_gpu_backend->vram_sync)
        g_gpu_backend->vram_sync(0, 0, VRAM_W, VRAM_H);
    STATE(s, g_vram);
    STATE(s, S);
    STATE(s, D);
    STATE(s, g_fifo);
    STATE(s, g_fifo_addr);
    STATE(s, g_gp0_src);
    STATE(s, g_fifo_n);
    STATE(s, g_fifo_need);
    STATE(s, g_polyline);
    STATE(s, g_wr);
    STATE(s, g_rd);
    state_heap(s, (void **)&g_wr_buf, &g_wr_count, &g_wr_cap, sizeof *g_wr_buf);
    STATE(s, g_line_last);
    STATE(s, g_line_flags);
    STATE(s, g_line_shaded);
    STATE(s, g_line_have_color);
    STATE(s, g_line_pending_color);
    STATE(s, g_ot_slot);
    STATE(s, g_ot_lo);
    STATE(s, g_ot_hi);
    STATE(s, g_ot_n);
    STATE(s, g_gpu_frame_counter);
    STATE(s, g_gpu_flip_counter);
    if (s->loading && !s->error && g_gpu_backend->vram_reload)
        g_gpu_backend->vram_reload();
}

void gpu_vblank(void) {
    ws_new_frame();
    if ((D.mode >> 5) & 1)
        D.field = !D.field;
    else
        D.field = false;
    g_gpu_frame_counter++;
    if (g_gpu_backend->end_frame)
        g_gpu_backend->end_frame();
}
