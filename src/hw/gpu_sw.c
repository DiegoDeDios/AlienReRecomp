/*
 * Software rasterizer backend: draws directly into g_vram with PS1 rules
 * (integer vertex coordinates, top-left fill convention, affine texture
 * mapping, CLUT lookup, texture windows, 4 semi-transparency modes,
 * dithering and mask bit handling). Serves as the accuracy reference for
 * the hardware renderer.
 */
#include <stdlib.h>

#include "gpu.h"
#include "psx.h"

static const int8_t k_dither[4][4] = {
    {-4, 0, -3, 1},
    {2, -2, 3, -1},
    {-3, 1, -4, 0},
    {3, -1, 2, -2},
};

typedef struct {
    const GpuState *s;
    uint32_t flags;
    int tx, ty;      /* texture page base */
    int depth;       /* 0=4bpp 1=8bpp 2=15bpp */
    int semi_mode;
    int cx, cy;      /* clut base */
    bool dither;
    uint8_t umask, vmask, uoff, voff;
} DrawCtx;

static void ctx_init(DrawCtx *d, const GpuState *s, uint32_t flags,
                     uint16_t clut, uint16_t tpage, bool allow_dither) {
    d->s = s;
    d->flags = flags;
    d->tx = (tpage & 0xF) * 64;
    d->ty = ((tpage >> 4) & 1) * 256;
    d->semi_mode = (tpage >> 5) & 3;
    d->depth = (tpage >> 7) & 3;
    if (d->depth == 3)
        d->depth = 2;
    d->cx = (clut & 0x3F) * 16;
    d->cy = (clut >> 6) & 0x1FF;
    d->umask = (uint8_t)(~(s->tw_mask_x * 8));
    d->vmask = (uint8_t)(~(s->tw_mask_y * 8));
    d->uoff = (uint8_t)((s->tw_off_x & s->tw_mask_x) * 8);
    d->voff = (uint8_t)((s->tw_off_y & s->tw_mask_y) * 8);
    bool modulated = (flags & PRIM_TEXTURED) && !(flags & PRIM_RAW);
    d->dither = allow_dither && s->dither &&
                ((flags & PRIM_SHADED) || modulated);
}

static inline uint16_t tex_fetch(const DrawCtx *d, uint8_t u, uint8_t v) {
    u = (uint8_t)((u & d->umask) | d->uoff);
    v = (uint8_t)((v & d->vmask) | d->voff);
    int y = (d->ty + v) & 511;
    switch (d->depth) {
    case 0: {
        uint16_t w = g_vram[y * VRAM_W + ((d->tx + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return g_vram[d->cy * VRAM_W + ((d->cx + idx) & 1023)];
    }
    case 1: {
        uint16_t w = g_vram[y * VRAM_W + ((d->tx + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return g_vram[d->cy * VRAM_W + ((d->cx + idx) & 1023)];
    }
    default:
        return g_vram[y * VRAM_W + ((d->tx + u) & 1023)];
    }
}

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Plot one pixel. r,g,b are 8-bit vertex colours; texel is used when
 * textured. */
static inline void plot(const DrawCtx *d, int x, int y, int r, int g, int b,
                        uint16_t texel, bool textured) {
    const GpuState *s = d->s;
    uint16_t *dst = &g_vram[y * VRAM_W + x];
    if (s->check_mask && (*dst & 0x8000))
        return;
    bool semi = (d->flags & PRIM_SEMI) != 0;
    int cr, cg, cb;
    uint16_t stp = 0;
    if (textured) {
        if (texel == 0)
            return;
        stp = texel & 0x8000;
        semi = semi && stp;
        int tr = (texel & 0x1F) << 3, tg = ((texel >> 5) & 0x1F) << 3,
            tb = ((texel >> 10) & 0x1F) << 3;
        if (d->flags & PRIM_RAW) {
            cr = tr, cg = tg, cb = tb;
        } else {
            cr = (tr * r) >> 7;
            cg = (tg * g) >> 7;
            cb = (tb * b) >> 7;
        }
    } else {
        cr = r, cg = g, cb = b;
    }
    if (d->dither) {
        int dv = k_dither[y & 3][x & 3];
        cr += dv, cg += dv, cb += dv;
    }
    cr = clamp255(cr) >> 3;
    cg = clamp255(cg) >> 3;
    cb = clamp255(cb) >> 3;
    if (semi) {
        int br = *dst & 0x1F, bg = (*dst >> 5) & 0x1F, bb = (*dst >> 10) & 0x1F;
        switch (d->semi_mode) {
        case 0: cr = (br + cr) >> 1; cg = (bg + cg) >> 1; cb = (bb + cb) >> 1; break;
        case 1: cr += br; cg += bg; cb += bb; break;
        case 2: cr = br - cr; cg = bg - cg; cb = bb - cb; break;
        case 3: cr = br + (cr >> 2); cg = bg + (cg >> 2); cb = bb + (cb >> 2); break;
        }
        cr = cr < 0 ? 0 : (cr > 31 ? 31 : cr);
        cg = cg < 0 ? 0 : (cg > 31 ? 31 : cg);
        cb = cb < 0 ? 0 : (cb > 31 ? 31 : cb);
    }
    uint16_t out = (uint16_t)(cr | (cg << 5) | (cb << 10));
    out |= stp;
    if (s->set_mask)
        out |= 0x8000;
    *dst = out;
}

/* ------------------------------------------------------------------ */
/* triangles                                                           */

typedef struct {
    int64_t dx, dy; /* 16.16 per-pixel steps */
    int64_t base;   /* value at (x0,y0) in 16.16 */
} Plane;

static Plane make_plane(const GVtx v[3], const int a[3], int64_t denom) {
    Plane p;
    int64_t a0 = a[0], a1 = a[1], a2 = a[2];
    int64_t x10 = v[1].x - v[0].x, y10 = v[1].y - v[0].y;
    int64_t x20 = v[2].x - v[0].x, y20 = v[2].y - v[0].y;
    int64_t nx = (a1 - a0) * y20 - (a2 - a0) * y10;
    int64_t ny = (a2 - a0) * x10 - (a1 - a0) * x20;
    p.dx = (nx << 16) / denom;
    p.dy = (ny << 16) / denom;
    p.base = (a0 << 16) + 0x8000; /* round to nearest when truncating */
    return p;
}

static inline int plane_at(const Plane *p, int64_t ox, int64_t oy) {
    return (int)((p->base + p->dx * ox + p->dy * oy) >> 16);
}

static void sw_triangle(const GpuState *s, const GVtx vin[3], uint32_t flags,
                        uint16_t clut, uint16_t tpage) {
    GVtx v[3] = {vin[0], vin[1], vin[2]};
    /* reject oversized primitives as the hardware does */
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        if (abs(v[i].x - v[j].x) >= 1024 || abs(v[i].y - v[j].y) >= 512)
            return;
    }
    int64_t area = (int64_t)(v[1].x - v[0].x) * (v[2].y - v[0].y) -
                   (int64_t)(v[2].x - v[0].x) * (v[1].y - v[0].y);
    if (area == 0)
        return;
    if (area < 0) {
        GVtx t = v[1];
        v[1] = v[2];
        v[2] = t;
        area = -area;
    }
    DrawCtx d;
    ctx_init(&d, s, flags, clut, tpage, true);
    bool shaded = flags & PRIM_SHADED;
    bool textured = flags & PRIM_TEXTURED;

    int minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    if (minx < s->area_x1) minx = s->area_x1;
    if (miny < s->area_y1) miny = s->area_y1;
    if (maxx > s->area_x2) maxx = s->area_x2;
    if (maxy > s->area_y2) maxy = s->area_y2;
    if (minx > maxx || miny > maxy)
        return;

    Plane pr, pg, pb, pu, pv;
    if (shaded) {
        int rr[3] = {v[0].r, v[1].r, v[2].r};
        int gg[3] = {v[0].g, v[1].g, v[2].g};
        int bb[3] = {v[0].b, v[1].b, v[2].b};
        pr = make_plane(v, rr, area);
        pg = make_plane(v, gg, area);
        pb = make_plane(v, bb, area);
    }
    if (textured) {
        int uu[3] = {v[0].u, v[1].u, v[2].u};
        int vv[3] = {v[0].v, v[1].v, v[2].v};
        pu = make_plane(v, uu, area);
        pv = make_plane(v, vv, area);
    }

    /* edge functions, oriented so that the interior is positive; the
     * top-left rule includes pixels exactly on top/left edges */
    int64_t ex[3], ey[3], ec[3];
    bool incl[3];
    for (int i = 0; i < 3; i++) {
        const GVtx *a = &v[i], *b = &v[(i + 1) % 3];
        ex[i] = -(int64_t)(b->y - a->y);
        ey[i] = (int64_t)(b->x - a->x);
        ec[i] = -(ex[i] * a->x + ey[i] * a->y);
        /* area > 0 means clockwise on a y-down screen: a "top" edge runs
         * left-to-right (dy==0, dx>0) and a "left" edge runs upwards
         * (dy<0). Pixels on top/left edges are drawn, right/bottom not. */
        int dyv = b->y - a->y, dxv = b->x - a->x;
        incl[i] = (dyv == 0 && dxv > 0) || (dyv < 0);
    }
    /* orientation check: interior must give positive values */
    {
        int64_t w = ex[0] * v[2].x + ey[0] * v[2].y + ec[0];
        if (w < 0) {
            for (int i = 0; i < 3; i++) {
                ex[i] = -ex[i];
                ey[i] = -ey[i];
                ec[i] = -ec[i];
                incl[i] = !incl[i];
            }
        }
    }
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            bool inside = true;
            for (int i = 0; i < 3 && inside; i++) {
                int64_t w = ex[i] * x + ey[i] * y + ec[i];
                if (w < 0 || (w == 0 && !incl[i]))
                    inside = false;
            }
            if (!inside)
                continue;
            int64_t ox = x - v[0].x, oy = y - v[0].y;
            int r, g, b;
            if (shaded) {
                r = plane_at(&pr, ox, oy);
                g = plane_at(&pg, ox, oy);
                b = plane_at(&pb, ox, oy);
            } else {
                r = v[0].r, g = v[0].g, b = v[0].b;
            }
            uint16_t texel = 0;
            if (textured) {
                int u = plane_at(&pu, ox, oy), vv = plane_at(&pv, ox, oy);
                texel = tex_fetch(&d, (uint8_t)u, (uint8_t)vv);
            }
            plot(&d, x, y, r, g, b, texel, textured);
        }
    }
}

/* ------------------------------------------------------------------ */
/* rectangles                                                          */

static void sw_rect(const GpuState *s, const GVtx *v, int w, int h,
                    uint32_t flags, uint16_t clut) {
    DrawCtx d;
    ctx_init(&d, s, flags, clut, s->tpage, false);
    bool textured = flags & PRIM_TEXTURED;
    for (int j = 0; j < h; j++) {
        int y = v->y + j;
        if (y < s->area_y1 || y > s->area_y2)
            continue;
        for (int i = 0; i < w; i++) {
            int x = v->x + i;
            if (x < s->area_x1 || x > s->area_x2)
                continue;
            uint16_t texel = 0;
            if (textured) {
                uint8_t u = (uint8_t)(s->rect_flip_x ? v->u - i : v->u + i);
                uint8_t vv = (uint8_t)(s->rect_flip_y ? v->v - j : v->v + j);
                texel = tex_fetch(&d, u, vv);
            }
            plot(&d, x & 1023, y & 511, v->r, v->g, v->b, texel, textured);
        }
    }
}

/* ------------------------------------------------------------------ */
/* lines                                                               */

static void sw_line(const GpuState *s, const GVtx *a, const GVtx *b,
                    uint32_t flags) {
    int dx = b->x - a->x, dy = b->y - a->y;
    if (abs(dx) >= 1024 || abs(dy) >= 512)
        return;
    DrawCtx d;
    ctx_init(&d, s, flags, 0, s->tpage, true);
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    bool shaded = flags & PRIM_SHADED;
    int64_t fx = ((int64_t)a->x << 16) + 0x8000, fy = ((int64_t)a->y << 16) + 0x8000;
    int64_t sx = steps ? ((int64_t)dx << 16) / steps : 0;
    int64_t sy = steps ? ((int64_t)dy << 16) / steps : 0;
    for (int k = 0; k <= steps; k++, fx += sx, fy += sy) {
        int x = (int)(fx >> 16), y = (int)(fy >> 16), r, g, bl;
        if (shaded && steps) {
            r = a->r + (b->r - a->r) * k / steps;
            g = a->g + (b->g - a->g) * k / steps;
            bl = a->b + (b->b - a->b) * k / steps;
        } else {
            r = a->r, g = a->g, bl = a->b;
        }
        if (x < s->area_x1 || x > s->area_x2 || y < s->area_y1 ||
            y > s->area_y2)
            continue;
        plot(&d, x, y, r, g, bl, 0, false);
    }
}

/* ------------------------------------------------------------------ */
/* VRAM operations                                                     */

static void sw_fill(int x, int y, int w, int h, uint16_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            g_vram[((y + j) & 511) * VRAM_W + ((x + i) & 1023)] = color;
}

static void sw_vram_write(const GpuState *s, int x, int y, int w, int h,
                          const uint16_t *data) {
    uint16_t mask_or = s->set_mask ? 0x8000 : 0;
    for (int j = 0; j < h; j++) {
        uint16_t *row = &g_vram[((y + j) & 511) * VRAM_W];
        for (int i = 0; i < w; i++) {
            uint16_t *p = &row[(x + i) & 1023];
            if (s->check_mask && (*p & 0x8000))
                continue;
            *p = data[j * w + i] | mask_or;
        }
    }
}

static void sw_vram_copy(const GpuState *s, int sx, int sy, int dx, int dy,
                         int w, int h) {
    uint16_t mask_or = s->set_mask ? 0x8000 : 0;
    uint16_t *tmp = malloc((size_t)w * 2);
    for (int j = 0; j < h; j++) {
        const uint16_t *src = &g_vram[((sy + j) & 511) * VRAM_W];
        for (int i = 0; i < w; i++)
            tmp[i] = src[(sx + i) & 1023];
        uint16_t *dst = &g_vram[((dy + j) & 511) * VRAM_W];
        for (int i = 0; i < w; i++) {
            uint16_t *p = &dst[(dx + i) & 1023];
            if (s->check_mask && (*p & 0x8000))
                continue;
            *p = tmp[i] | mask_or;
        }
    }
    free(tmp);
}

const GpuBackend g_gpu_sw = {
    .name = "software",
    .triangle = sw_triangle,
    .rect = sw_rect,
    .line = sw_line,
    .fill = sw_fill,
    .vram_write = sw_vram_write,
    .vram_copy = sw_vram_copy,
    .vram_sync = NULL,
    .end_frame = NULL,
};
