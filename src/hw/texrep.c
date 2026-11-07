#include "texrep.h"

#include <stddef.h>

#include "widescreen.h"

#define MAX_GROUPS  64
#define MAX_SOURCES 512
#define TPAGE_MASK  0x19Fu /* page and depth: not the blend mode */
#define FULL_COLOR  128    /* a vertex colour of 128 leaves a texel as it is */
#define DISPLAY_W   368.0f /* columns of the buffer the game's display mode shows */
#define DISPLAY_H   240.0f
#define ART_MIDDLE  192.0f /* the game centres 2D art here, not on the display's middle */

typedef struct Group {
    int image;
    float aspect;
    TexrepPlace place;
    int x, y, w, h;
    int peak, last_peak; /* brightest member this frame and the last one drawn */
    bool drawn;
} Group;

typedef struct Source {
    int group;
    uint16_t tpage, clut;
    bool any_clut;
    int u, v, w, h;
} Source;

bool g_texrep_on = true;

static Group g_groups[MAX_GROUPS];
static int g_ngroups;
static Source g_sources[MAX_SOURCES];
static int g_nsources;

int texrep_add_group(int image, float aspect, TexrepPlace place, const int *at) {
    if (g_ngroups >= MAX_GROUPS)
        return -1;
    Group *g = &g_groups[g_ngroups];
    *g = (Group){.image = image, .aspect = aspect, .place = place};
    if (place == TEXREP_SCREEN)
        g->x = at[0], g->y = at[1], g->w = at[2], g->h = at[3];
    if (place == TEXREP_FILL)
        g->y = at[0], g->h = at[1];
    return g_ngroups++;
}

bool texrep_add_source(int group, int page_x, int page_y, int bits, int clut_x, int clut_y, int u,
                       int v, int w, int h) {
    if (g_nsources >= MAX_SOURCES || group < 0 || group >= g_ngroups || w <= 0 || h <= 0)
        return false;
    int depth = bits == 4 ? 0 : bits == 8 ? 1 : 2;
    g_sources[g_nsources++] = (Source){
        .group = group,
        .tpage = (uint16_t)((page_x / 64) | ((page_y / 256) << 4) | (depth << 7)),
        .clut = (uint16_t)((clut_x / 16) | (clut_y << 6)),
        .any_clut = depth == 2,
        .u = u, .v = v, .w = w, .h = h,
    };
    return true;
}

/* A band's sources are told apart by where they are drawn: the menus use the same
 * textures for the top and the bottom one. */
static bool drawn_in_rows(const Group *g, const GpuState *s, const GVtx q[4]) {
    int mid = (q[0].y + q[3].y) / 2 - s->area_y1;
    return g->place != TEXREP_FILL || (mid >= g->y && mid < g->y + g->h);
}

/* the source a quad's texels lie in */
static const Source *find_source(const GpuState *s, const GVtx q[4], uint16_t tpage, uint16_t clut) {
    int u0 = q[0].u, u1 = q[0].u, v0 = q[0].v, v1 = q[0].v;
    for (int i = 1; i < 4; i++) {
        if (q[i].u < u0) u0 = q[i].u;
        if (q[i].u > u1) u1 = q[i].u;
        if (q[i].v < v0) v0 = q[i].v;
        if (q[i].v > v1) v1 = q[i].v;
    }
    for (int i = 0; i < g_nsources; i++) {
        const Source *r = &g_sources[i];
        if ((tpage & TPAGE_MASK) == r->tpage && (r->any_clut || clut == r->clut) && u0 >= r->u &&
            v0 >= r->v && u1 <= r->u + r->w && v1 <= r->v + r->h &&
            drawn_in_rows(&g_groups[r->group], s, q))
            return r;
    }
    return NULL;
}

static uint8_t scaled_color(int c) {
    int v = c * 255 / FULL_COLOR;
    return (uint8_t)(v > 255 ? 255 : v);
}

static void draw_sprite(const GpuState *s, const Source *r, int image, const GVtx q[4], uint32_t flags) {
    float xy[8], uv[8];
    for (int i = 0; i < 4; i++) {
        xy[i * 2] = (float)q[i].x;
        xy[i * 2 + 1] = (float)q[i].y;
        uv[i * 2] = (float)(q[i].u - r->u) / (float)r->w;
        uv[i * 2 + 1] = (float)(q[i].v - r->v) / (float)r->h;
    }
    uint8_t rgba[4] = {255, 255, 255, 255};
    if (!(flags & PRIM_RAW)) {
        rgba[0] = scaled_color(q[0].r);
        rgba[1] = scaled_color(q[0].g);
        rgba[2] = scaled_color(q[0].b);
    }
    g_gpu_backend->image_quad(s, image, xy, uv, flags, rgba);
}

/* The layers of a picture fade by their colours, and the first one drawn may be a
 * dim one, so the image follows the brightest layer of the last frame. */
static void draw_at_screen(const GpuState *s, Group *g, uint32_t flags) {
    float x0 = (float)(g->x + s->area_x1), y0 = (float)(g->y + s->area_y1);
    float x1 = x0 + (float)g->w, y1 = y0 + (float)g->h;
    const float xy[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
    static const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    int peak = g->last_peak ? g->last_peak : g->peak;
    const uint8_t rgba[4] = {255, 255, 255, scaled_color(peak)};
    g_gpu_backend->image_quad(s, g->image, xy, uv, flags, rgba);
}

/* Across the window at the image's own proportions, its middle on the game's 2D art:
 * a wide image loses its sides in a narrower window, and in a wider one the edge
 * columns carry on (the sampler clamps). The quad is widened so that the renderer's
 * squash lands it on the edges. Upside down if the game draws these sources so;
 * dimmed by their colour. */
static void draw_filling(const GpuState *s, Group *g, const GVtx q[4], uint32_t flags) {
    bool wide = flags & PRIM_WS_2D;
    float k = wide ? g_ws_k : 1.0f, window = wide ? g_ws_aspect : 4.0f / 3.0f;
    float edge0 = (float)s->area_x1, edge1 = edge0 + DISPLAY_W;
    float x0 = wide ? s->ws_anchor + (edge0 - s->ws_anchor) / k : edge0;
    float x1 = wide ? s->ws_anchor + (edge1 - s->ws_anchor) / k : edge1;
    float y0 = (float)(g->y + s->area_y1), y1 = y0 + (float)g->h;
    float part = window / (g->aspect * (float)g->h / DISPLAY_H);
    float u0 = 0.5f - part * ART_MIDDLE / DISPLAY_W;
    float u1 = 0.5f + part * (DISPLAY_W - ART_MIDDLE) / DISPLAY_W;
    bool flip = (q[2].y - q[0].y) * (q[2].v - q[0].v) < 0;
    float v0 = flip ? 1.0f : 0.0f, v1 = flip ? 0.0f : 1.0f;
    const float xy[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
    const float uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
    int peak = g->last_peak ? g->last_peak : g->peak;
    uint8_t c = scaled_color(peak);
    const uint8_t rgba[4] = {c, c, c, 255};
    g_gpu_backend->image_quad(s, g->image, xy, uv, flags, rgba);
}

bool texrep_quad(const GpuState *s, const GVtx q[4], uint16_t tpage, uint16_t clut, uint32_t flags) {
    if (!g_texrep_on || !g_nsources || !g_gpu_backend->image_quad)
        return false;
    const Source *r = find_source(s, q, tpage, clut);
    if (!r)
        return false;
    Group *g = &g_groups[r->group];
    if (g->place == TEXREP_SPRITE) {
        draw_sprite(s, r, g->image, q, flags);
        return true;
    }
    int bright = q[0].r > q[0].g ? q[0].r : q[0].g;
    bright = bright > q[0].b ? bright : q[0].b;
    if (bright > g->peak)
        g->peak = bright;
    if (!g->drawn && g->place == TEXREP_FILL)
        draw_filling(s, g, q, flags);
    else if (!g->drawn)
        draw_at_screen(s, g, flags);
    g->drawn = true;
    return true;
}

void texrep_new_frame(void) {
    for (int i = 0; i < g_ngroups; i++) {
        Group *g = &g_groups[i];
        if (g->peak)
            g->last_peak = g->peak;
        g->peak = 0;
        g->drawn = false;
    }
}
