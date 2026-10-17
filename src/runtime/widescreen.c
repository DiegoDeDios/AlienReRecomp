#include "widescreen.h"

#include <string.h>

bool g_ws_enabled;
bool g_ws_wide_display;
uint32_t g_ws_3d_count;
float g_ws_aspect = 4.0f / 3.0f;
float g_ws_k = 1.0f;
int32_t g_ws_k_fp = 0x10000;

/* Two generations of an open-addressing set of packed (x, y) points. */
#define SET_BITS 15
#define SET_SIZE (1u << SET_BITS)
#define EMPTY 0xFFFFFFFFu

static uint32_t g_set[2][SET_SIZE];
static uint32_t g_count[2];
static int g_cur;

void ws_configure(float aspect) {
    g_ws_aspect = aspect;
    g_ws_enabled = aspect > 4.0f / 3.0f + 0.01f;
    g_ws_k = g_ws_enabled ? (4.0f / 3.0f) / aspect : 1.0f;
    g_ws_k_fp = (int32_t)(g_ws_k * 65536.0f + 0.5f);
    memset(g_set, 0xFF, sizeof g_set);
    g_count[0] = g_count[1] = 0;
}

static inline uint32_t key(int32_t x, int32_t y) {
    return ((uint32_t)x & 0xFFFF) | ((uint32_t)y << 16);
}

static inline uint32_t hash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    return k & (SET_SIZE - 1);
}

void ws_record(int32_t x, int32_t y) {
    uint32_t k = key(x, y);
    uint32_t *s = g_set[g_cur];
    if (g_count[g_cur] > SET_SIZE * 3 / 4)
        return; /* full: very unusual, extra points just go unrecorded */
    for (uint32_t h = hash(k);; h = (h + 1) & (SET_SIZE - 1)) {
        if (s[h] == k)
            return;
        if (s[h] == EMPTY) {
            s[h] = k;
            g_count[g_cur]++;
            return;
        }
    }
}

static bool lookup(int g, uint32_t k) {
    const uint32_t *s = g_set[g];
    for (uint32_t h = hash(k);; h = (h + 1) & (SET_SIZE - 1)) {
        if (s[h] == k)
            return true;
        if (s[h] == EMPTY)
            return false;
    }
}

bool ws_projected_exact(int32_t x, int32_t y) {
    uint32_t k = key(x, y);
    return lookup(0, k) || lookup(1, k);
}

bool ws_projected(int32_t x, int32_t y) {
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            uint32_t k = key(x + dx, y + dy);
            if (lookup(0, k) || lookup(1, k))
                return true;
        }
    return false;
}

/* frames without 3D before falling back to a 4:3 display (the game renders
 * at 30 fps or lower, so single frames without drawing are normal) */
#define WS_HOLD_FRAMES 12

void ws_new_frame(void) {
    static uint32_t idle;
    if (g_ws_3d_count)
        idle = 0;
    else if (idle < WS_HOLD_FRAMES)
        idle++;
    g_ws_wide_display = g_ws_enabled && idle < WS_HOLD_FRAMES;
    g_ws_3d_count = 0;
    g_cur ^= 1;
    if (g_count[g_cur]) {
        memset(g_set[g_cur], 0xFF, sizeof g_set[g_cur]);
        g_count[g_cur] = 0;
    }
}
