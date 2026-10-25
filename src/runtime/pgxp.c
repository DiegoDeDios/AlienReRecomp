#include "pgxp.h"

#include "psx.h"

bool g_pgxp;

static PgxpVtx g_gte[3];          /* SXY0..SXY2 */
static PgxpVtx g_gpr[32];
static PgxpVtx g_ram_sh[0x200000 / 4];
static PgxpVtx g_spad_sh[0x400 / 4];

static inline PgxpVtx *mem_slot(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u)
        return &g_ram_sh[(p & 0x1FFFFC) >> 2];
    if ((p & 0xFFFFFC00u) == 0x1F800000u)
        return &g_spad_sh[(p & 0x3FC) >> 2];
    return NULL;
}

void pgxp_reset(void) {
    /* w = 0 marks an entry as unknown */
    memset(g_gte, 0, sizeof g_gte);
    memset(g_gpr, 0, sizeof g_gpr);
    memset(g_ram_sh, 0, sizeof g_ram_sh);
    memset(g_spad_sh, 0, sizeof g_spad_sh);
}

void pgxp_gte_push(float x, float y, float w, uint32_t raw) {
    g_gte[0] = g_gte[1];
    g_gte[1] = g_gte[2];
    g_gte[2] = (PgxpVtx){x, y, w, raw};
}

void pgxp_gte_set(int reg, const PgxpVtx *v) {
    if (reg == 15) { /* SXYP pushes the FIFO */
        g_gte[0] = g_gte[1];
        g_gte[1] = g_gte[2];
        g_gte[2] = *v;
    } else if (reg >= 12 && reg <= 14) {
        g_gte[reg - 12] = *v;
    }
}

/* NCLIP (the winding of SXY0..SXY2, used for back-face culling) goes
 * wrong when a vertex is at or behind the camera or far off-screen: the
 * GTE clamps those to the edge of its range, the integer coordinates no
 * longer describe the polygon, and the sign can cull a visible
 * near-camera polygon or keep a back face. The homogeneous positions give
 * the true winding: det[x y w] = w0·w1·w2 × the screen-space value, and
 * its sign stays the orientation of the triangle as seen from the eye
 * when some w <= 0. Only those cases are corrected (keeping the integer
 * magnitude); everything else gets the hardware result. */
int64_t pgxp_nclip(int64_t n, const uint32_t sxy[3]) {
    const PgxpVtx *v = g_gte;
    bool clamped = false;
    for (int i = 0; i < 3; i++) {
        float sx, sy;
        if (v[i].raw != sxy[i] || v[i].w == 0.0f)
            return n;
        if (!pgxp_screen(&v[i], &sx, &sy) || sx < -1024.0f || sx > 1023.0f ||
            sy < -1024.0f || sy > 1023.0f)
            clamped = true;
    }
    if (!clamped)
        return n;
    double det = (double)v[0].x * ((double)v[1].y * v[2].w - (double)v[1].w * v[2].y) -
                 (double)v[0].y * ((double)v[1].x * v[2].w - (double)v[1].w * v[2].x) +
                 (double)v[0].w * ((double)v[1].x * v[2].y - (double)v[1].y * v[2].x);
    if (det == 0.0)
        return n;
    int64_t m = n < 0 ? -n : n;
    if (m == 0)
        m = 1;
    return det > 0.0 ? m : -m;
}

bool pgxp_lookup(uint32_t addr, uint32_t word, PgxpVtx *out) {
    if (!addr)
        return false;
    PgxpVtx *s = mem_slot(addr);
    if (!s || s->raw != word || s->w == 0.0f)
        return false;
    *out = *s;
    return true;
}

/* --- recompiler hooks ------------------------------------------------ */

void pgxp_mfc2(int rt, int rd) {
    if (rt == 0)
        return;
    int i = rd == 15 ? 2 : rd - 12;
    g_gpr[rt] = g_gte[i];
}

void pgxp_mtc2(int rd, int rt) {
    PgxpVtx v = g_gpr[rt];
    if (v.raw != g_cpu->r[rt])
        v.raw = ~g_cpu->r[rt];
    pgxp_gte_set(rd, &v);
}

void pgxp_swc2(uint32_t addr, int rd) {
    PgxpVtx *s = mem_slot(addr);
    if (s)
        *s = g_gte[rd == 15 ? 2 : rd - 12];
}

void pgxp_lwc2(int rd, uint32_t addr) {
    PgxpVtx *s = mem_slot(addr);
    PgxpVtx v = s ? *s : (PgxpVtx){0, 0, 0, 0};
    uint32_t cur = mem_r32(addr);
    if (v.raw != cur)
        v.raw = ~cur;
    pgxp_gte_set(rd, &v);
}

void pgxp_lw(int rt, uint32_t addr) {
    if (rt == 0)
        return;
    PgxpVtx *s = mem_slot(addr);
    if (s)
        g_gpr[rt] = *s;
}

void pgxp_sw(uint32_t addr, int rt) {
    PgxpVtx *s = mem_slot(addr);
    if (!s)
        return;
    if (rt != 0 && g_gpr[rt].raw == g_cpu->r[rt])
        *s = g_gpr[rt];
    else
        s->raw = ~mem_r32(addr); /* overwritten by an untracked value */
}

void pgxp_mov(int rd, int rs) {
    if (rd != 0)
        g_gpr[rd] = g_gpr[rs];
}
