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
    float x, y;    /* precise screen coordinates (same space as SXY) */
    float z;       /* depth (> 0), 0 = unknown */
    uint32_t raw;  /* the packed SXY value this entry describes */
} PgxpVtx;

extern bool g_pgxp;

/* Forget all shadow values. Needed when tracking is switched back on while
 * the game runs: entries left from before could still match the raw value
 * in RAM but describe an older vertex. */
void pgxp_reset(void);

/* GTE side */
void pgxp_gte_push(float x, float y, float z, uint32_t raw, bool valid);
void pgxp_gte_set(int reg, const PgxpVtx *v);

/* lookup for the GPU: precise vertex for a packet word read from `addr` */
bool pgxp_lookup(uint32_t addr, uint32_t word, PgxpVtx *out);

/* hooks used by the recompiled code (see recomp.h) */
void pgxp_mfc2(int rt, int rd);
void pgxp_mtc2(int rd, int rt);
void pgxp_swc2(uint32_t addr, int rd);
void pgxp_lwc2(int rd, uint32_t addr);
void pgxp_lw(int rt, uint32_t addr);
void pgxp_sw(uint32_t addr, int rt);
void pgxp_mov(int rd, int rs);
