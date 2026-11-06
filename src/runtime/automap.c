#include "automap.h"

#include <math.h>
#include <string.h>

#include "psx.h"

/* Level section map in RAM (the MAP chunk with its offsets made pointers, see
 * FINDINGS.md). Each room has a bounding box and a BSP of 8-byte nodes; a slice
 * through the BSP at one height gives the regions open at that height, and an
 * edge of one with solid behind it is a wall. */
#define MAP_PTR        0x800A4758u
#define PLAYER_PTR     0x800A49E8u
#define DEMO_PLAYBACK  0x800A48C4u
#define RAM_SIZE       0x200000u
#define AUTOMAP_PI     3.14159265f

#define MAP_PLANES     16
#define MAP_NODES      20          /* the first hull: the cleanest outline */
#define MAP_ROOMS      32
#define MAP_ID         40          /* room count and extents: differ per section */
#define MAP_ID_END     64
#define MAP_ROOM_COUNT 50
#define ROOM_SIZE      40
#define ROOM_NODE_BASE 12
#define ROOM_BOX       28

#define LEAF_SOLID     (-2)
#define FIXED_ONE      4096.0f

#define MAX_ROOMS      32          /* 31 on the disc */
#define MAX_SECTIONS   128         /* 90 on the disc */
#define MAX_SEGS       4096
#define MAX_POLY       24
#define MAX_DEPTH      128
#define MAX_STEPS      2000

#define SLICE_ABOVE    30.0f       /* above the player's origin: clear of low obstacles */
#define NEW_FLOOR      48.0f       /* climbed this far: slice again */
#define PROBE          2.0f        /* how far behind an edge to look for solid */
#define MIN_EDGE       0.5f
#define PLAY_TIMEOUT   30          /* vblanks */

typedef struct Plane { float nx, ny, nz, d; } Plane;
typedef struct Box { float x0, y0, z0, x1, y1, z1; } Box;
typedef struct Poly { float x[MAX_POLY], z[MAX_POLY]; int n; } Poly;
typedef struct Section { uint32_t id, seen; } Section;

static Section g_sections[MAX_SECTIONS];
static AutomapSeg g_segs[MAX_SEGS];
static int g_nsegs;
static uint32_t g_slice_id, g_slice_seen;
static float g_slice_y = NAN;
static bool g_hud_drawn;
static int g_hud_age = PLAY_TIMEOUT;

static uint32_t g_map, g_planes, g_nodes, g_rooms;
static int g_room_count;

static bool in_ram(uint32_t a) { return a != 0 && (a & 0x1FFFFFFFu) < RAM_SIZE; }

static bool map_open(void) {
    g_map = mem_r32(MAP_PTR);
    if (!in_ram(g_map))
        return false;
    g_planes = mem_r32(g_map + MAP_PLANES);
    g_nodes = mem_r32(g_map + MAP_NODES);
    g_rooms = mem_r32(g_map + MAP_ROOMS);
    g_room_count = mem_r16(g_map + MAP_ROOM_COUNT);
    return in_ram(g_planes) && in_ram(g_nodes) && in_ram(g_rooms) && g_room_count > 0 &&
           g_room_count <= MAX_ROOMS;
}

static uint32_t map_id(void) {
    uint32_t h = 2166136261u;
    for (uint32_t i = MAP_ID; i < MAP_ID_END; i += 4)
        h = (h ^ mem_r32(g_map + i)) * 16777619u;
    return h;
}

static Section *section(uint32_t id) {
    for (int i = 0; i < MAX_SECTIONS; i++) {
        if (g_sections[i].id == id)
            return &g_sections[i];
        if (!g_sections[i].id) {
            g_sections[i].id = id;
            return &g_sections[i];
        }
    }
    return NULL;
}

static uint32_t room_addr(int r) { return g_rooms + (uint32_t)r * ROOM_SIZE; }

static uint32_t room_nodes(int r) {
    return g_nodes + (uint32_t)mem_r16(room_addr(r) + ROOM_NODE_BASE) * 8;
}

static Box room_box(int r) {
    uint32_t a = room_addr(r) + ROOM_BOX;
    return (Box){(int16_t)mem_r16(a),     (int16_t)mem_r16(a + 2),  (int16_t)mem_r16(a + 4),
                 (int16_t)mem_r16(a + 6), (int16_t)mem_r16(a + 8), (int16_t)mem_r16(a + 10)};
}

static bool box_has(const Box *b, float x, float y, float z) {
    return x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1 && z >= b->z0 && z <= b->z1;
}

/* the node's plane: on it, nx x + ny y + nz z + d = 0 */
static Plane node_plane(uint32_t node) {
    int32_t a = (int32_t)mem_r32(node);
    int32_t v = a >> 2;
    float d = (float)v / FIXED_ONE;
    switch (a & 3) {
    case 0: return (Plane){1, 0, 0, d};
    case 1: return (Plane){0, 1, 0, d};
    case 2: return (Plane){0, 0, 1, d};
    default: break;
    }
    uint32_t p = g_planes + (uint32_t)v * 12;
    return (Plane){(float)(int16_t)mem_r16(p) / FIXED_ONE, (float)(int16_t)mem_r16(p + 2) / FIXED_ONE,
                   (float)(int16_t)mem_r16(p + 4) / FIXED_ONE,
                   (float)(int32_t)mem_r32(p + 8) / FIXED_ONE};
}

static int node_child(uint32_t node, bool back) {
    return (int16_t)mem_r16(node + (back ? 6 : 4));
}

static int room_leaf(int r, float x, float y, float z) {
    uint32_t base = room_nodes(r);
    int i = 0;
    for (int step = 0; i >= 0 && step < MAX_STEPS; step++) {
        uint32_t node = base + (uint32_t)i * 8;
        Plane p = node_plane(node);
        i = node_child(node, p.nx * x + p.ny * y + p.nz * z + p.d >= 0);
    }
    return i < 0 ? i : LEAF_SOLID;
}

/* Besides -1 (open) the leaves hold other negative values, mostly slivers along
 * the seams between rooms. Counted as walls they would draw lines along the
 * seams, so only -2 is solid here. */
static bool is_solid(float x, float y, float z) {
    for (int r = 0; r < g_room_count; r++) {
        Box b = room_box(r);
        if (box_has(&b, x, y, z) && room_leaf(r, x, y, z) != LEAF_SOLID)
            return false;
    }
    return true;
}

/* the part of `in` where a x + b z + c is <= 0 (keep_negative) or >= 0 */
static void poly_clip(const Poly *in, float a, float b, float c, bool keep_negative, Poly *out) {
    out->n = 0;
    for (int i = 0; i < in->n; i++) {
        int j = (i + 1) % in->n;
        float vi = a * in->x[i] + b * in->z[i] + c;
        float vj = a * in->x[j] + b * in->z[j] + c;
        bool ki = keep_negative ? vi <= 0 : vi >= 0;
        bool kj = keep_negative ? vj <= 0 : vj >= 0;
        if (ki && out->n < MAX_POLY) {
            out->x[out->n] = in->x[i];
            out->z[out->n++] = in->z[i];
        }
        if (ki != kj && out->n < MAX_POLY) {
            float t = vi / (vi - vj);
            out->x[out->n] = in->x[i] + t * (in->x[j] - in->x[i]);
            out->z[out->n++] = in->z[i] + t * (in->z[j] - in->z[i]);
        }
    }
}

static void add_walls(const Poly *p, float y) {
    float cx = 0, cz = 0;
    for (int i = 0; i < p->n; i++)
        cx += p->x[i], cz += p->z[i];
    cx /= (float)p->n, cz /= (float)p->n;
    for (int i = 0; i < p->n && g_nsegs < MAX_SEGS; i++) {
        int j = (i + 1) % p->n;
        float dx = p->x[j] - p->x[i], dz = p->z[j] - p->z[i];
        float len = sqrtf(dx * dx + dz * dz);
        if (len < MIN_EDGE)
            continue;
        float nx = -dz / len, nz = dx / len;
        float mx = (p->x[i] + p->x[j]) * 0.5f, mz = (p->z[i] + p->z[j]) * 0.5f;
        if (nx * (cx - mx) + nz * (cz - mz) > 0)
            nx = -nx, nz = -nz;
        if (is_solid(mx + nx * PROBE, y, mz + nz * PROBE))
            g_segs[g_nsegs++] = (AutomapSeg){p->x[i], p->z[i], p->x[j], p->z[j]};
    }
}

static void slice_node(uint32_t base, int i, const Poly *poly, float y, int depth) {
    if (poly->n < 3 || depth > MAX_DEPTH)
        return;
    if (i < 0) {
        if (i != LEAF_SOLID)
            add_walls(poly, y);
        return;
    }
    uint32_t node = base + (uint32_t)i * 8;
    Plane p = node_plane(node);
    float c = p.ny * y + p.d;
    if (p.nx == 0 && p.nz == 0) {
        slice_node(base, node_child(node, c >= 0), poly, y, depth + 1);
        return;
    }
    Poly side;
    poly_clip(poly, p.nx, p.nz, c, true, &side);
    slice_node(base, node_child(node, false), &side, y, depth + 1);
    poly_clip(poly, p.nx, p.nz, c, false, &side);
    slice_node(base, node_child(node, true), &side, y, depth + 1);
}

static void slice_rooms(uint32_t seen, float y) {
    g_nsegs = 0;
    for (int r = 0; r < g_room_count; r++) {
        Box b = room_box(r);
        if (!(seen & (1u << r)) || y < b.y0 || y > b.y1)
            continue;
        Poly p = {{b.x0, b.x1, b.x1, b.x0}, {b.z0, b.z0, b.z1, b.z1}, 4};
        slice_node(room_nodes(r), 0, &p, y, 0);
    }
}

static bool player_pos(float *x, float *y, float *z) {
    uint32_t p = mem_r32(PLAYER_PTR);
    if (!in_ram(p))
        return false;
    *x = (float)(int32_t)mem_r32(p + 0x30) / FIXED_ONE;
    *y = (float)(int32_t)mem_r32(p + 0x34) / FIXED_ONE;
    *z = (float)(int32_t)mem_r32(p + 0x38) / FIXED_ONE;
    return true;
}

void automap_game_frame(void) {
    float x, y, z;
    if (mem_r32(DEMO_PLAYBACK) || !map_open() || !player_pos(&x, &y, &z))
        return;
    g_hud_drawn = true;
    Section *s = section(map_id());
    if (!s)
        return;
    for (int r = 0; r < g_room_count; r++) {
        Box b = room_box(r);
        if (box_has(&b, x, y, z) && room_leaf(r, x, y, z) != LEAF_SOLID)
            s->seen |= 1u << r;
    }
}

bool automap_view(AutomapView *view) {
    g_hud_age = g_hud_drawn ? 0 : g_hud_age + 1;
    g_hud_drawn = false;
    float x, y, z;
    if (g_hud_age >= PLAY_TIMEOUT || !map_open() || !player_pos(&x, &y, &z))
        return false;
    uint32_t id = map_id();
    Section *s = section(id);
    uint32_t seen = s ? s->seen : 0;
    if (id != g_slice_id || seen != g_slice_seen || !(fabsf(y - g_slice_y) < NEW_FLOOR)) {
        g_slice_id = id;
        g_slice_seen = seen;
        g_slice_y = y;
        slice_rooms(seen, y - SLICE_ABOVE);
    }
    uint32_t p = mem_r32(PLAYER_PTR);
    *view = (AutomapView){g_segs, g_nsegs, x, z,
                          (float)mem_r16(p + 0x3E) * 2.0f * AUTOMAP_PI / FIXED_ONE};
    return true;
}
