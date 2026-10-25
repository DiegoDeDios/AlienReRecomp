/*
 * PGXP-style precision geometry.
 *
 * The GTE rounds projected vertices to integer pixels and the GPU maps
 * textures affinely: together they cause the characteristic PS1 wobble and
 * texture warping. Here every RTPS/RTPT result also gets a high-precision
 * screen position and depth. That shadow value follows the vertex as the
 * game moves it through CPU registers and RAM (hooks emitted by the
 * recompiler), and when a polygon packet reaches the GPU the renderer looks
 * the vertex up by its RAM address. A shadow entry is only trusted while it
 * still describes the value actually stored there (raw-value check), so
 * stale entries never leak into rendering.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct PgxpVtx {
    /* homogeneous screen position: the vertex projects to (x / w, y / w),
     * the space of SXY. w is the view depth; it is <= 0 at or behind the
     * camera, where the GTE's own result is clamped garbage. w = 0 marks
     * an unknown entry. */
    float x, y, w;
    uint32_t raw;  /* the packed SXY value this entry describes */
} PgxpVtx;

/* nearest depth at which a projection is used as a screen position; the
 * renderer clips polygons with vertices nearer than this */
#define PGXP_NEAR 1.0f

extern bool g_pgxp;

/* Forget all shadow values. Needed when tracking is switched back on while
 * the game runs: entries left from before could still match the raw value
 * in RAM but describe an older vertex. */
void pgxp_reset(void);

/* GTE side */
void pgxp_gte_push(float x, float y, float w, uint32_t raw);
void pgxp_gte_set(int reg, const PgxpVtx *v);
/* NCLIP of SXY0..SXY2 with the integer result `n`, corrected (see pgxp.c) */
int64_t pgxp_nclip(int64_t n, const uint32_t sxy[3]);

/* lookup for the GPU: the tracked projection of a packet word read from
 * `addr` (anywhere relative to the camera) */
bool pgxp_lookup(uint32_t addr, uint32_t word, PgxpVtx *out);

/* screen position of a tracked projection, if it is in front of the camera
 * and not absurdly far off-screen */
static inline bool pgxp_screen(const PgxpVtx *v, float *x, float *y) {
    if (!(v->w >= PGXP_NEAR))
        return false;
    float sx = v->x / v->w, sy = v->y / v->w;
    if (!(sx > -8192.0f && sx < 8192.0f && sy > -8192.0f && sy < 8192.0f))
        return false;
    *x = sx;
    *y = sy;
    return true;
}

/* hooks used by the recompiled code (see recomp.h) */
void pgxp_mfc2(int rt, int rd);
void pgxp_mtc2(int rd, int rt);
void pgxp_swc2(uint32_t addr, int rd);
void pgxp_lwc2(int rd, uint32_t addr);
void pgxp_lw(int rt, uint32_t addr);
void pgxp_sw(uint32_t addr, int rt);
void pgxp_mov(int rd, int rs);
