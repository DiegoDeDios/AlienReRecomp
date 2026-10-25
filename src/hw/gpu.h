/*
 * GPU internals shared between the command processor (gpu.c) and the
 * rendering backends (software rasterizer, hardware renderer).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VRAM_W 1024
#define VRAM_H 512

typedef struct GpuState {
    /* E1 draw mode */
    uint16_t tpage;        /* bits 0-8 texpage + bit 11 tex disable */
    bool dither;
    bool draw_to_display;
    bool tex_disable;
    bool rect_flip_x, rect_flip_y;
    /* E2 texture window (in 8-pixel units) */
    uint8_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    /* E3/E4 drawing area (inclusive) */
    int area_x1, area_y1, area_x2, area_y2;
    /* E5 drawing offset */
    int off_x, off_y;
    /* E6 mask */
    bool set_mask, check_mask;
    /* widescreen: horizontal anchor (VRAM x) for PRIM_WS_* primitives */
    float ws_anchor;
} GpuState;

typedef struct GVtx {
    int32_t x, y;
    uint8_t r, g, b;
    uint8_t u, v;
    /* PGXP: precise position (VRAM coordinates, drawing offset applied)
     * and depth, when the vertex is a tracked GTE projection in front of
     * the camera */
    bool precise;
    float fx, fy, fz;
    /* PGXP: any tracked GTE projection, also at or behind the camera:
     * homogeneous VRAM position (hx / hw, hy / hw), hw = depth */
    bool tracked;
    float hx, hy, hw;
} GVtx;

enum {
    PRIM_SHADED = 1 << 0,   /* gouraud */
    PRIM_TEXTURED = 1 << 1,
    PRIM_SEMI = 1 << 2,     /* semi-transparent */
    PRIM_RAW = 1 << 3,      /* raw texture (no modulation) */
    /* widescreen classification (see runtime/widescreen.h) */
    PRIM_WS_2D = 1 << 8,     /* not from the GTE: squash around screen centre */
    PRIM_WS_SPRITE = 1 << 9, /* 2D shape centred on a projected point:
                                squash around its own centre */
};

typedef struct GpuBackend {
    const char *name;
    void (*triangle)(const GpuState *s, const GVtx v[3], uint32_t flags,
                     uint16_t clut, uint16_t tpage);
    void (*rect)(const GpuState *s, const GVtx *v, int w, int h,
                 uint32_t flags, uint16_t clut);
    void (*line)(const GpuState *s, const GVtx *a, const GVtx *b,
                 uint32_t flags);
    void (*fill)(int x, int y, int w, int h, uint16_t color);
    void (*vram_write)(const GpuState *s, int x, int y, int w, int h,
                       const uint16_t *data);
    void (*vram_copy)(const GpuState *s, int sx, int sy, int dx, int dy,
                      int w, int h);
    /* make the CPU-visible VRAM copy current for the given rectangle */
    void (*vram_sync)(int x, int y, int w, int h);
    void (*end_frame)(void);
    /* g_vram was replaced as a whole (save state loaded): take it over */
    void (*vram_reload)(void);
} GpuBackend;

extern uint16_t g_vram[VRAM_W * VRAM_H];
extern const GpuBackend g_gpu_sw;
extern const GpuBackend *g_gpu_backend;

static inline int32_t sext11(int32_t v) { return (int32_t)((uint32_t)v << 21) >> 21; }
