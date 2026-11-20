#include "geometry_clip.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

static double distance(const GeometryVertex *v, int plane, double bound) {
    switch (plane) {
    case 0: return v->z - GEOMETRY_NEAR;
    case 1: return v->hx - bound * v->z;
    case 2: return bound * v->z - v->hx;
    case 3: return v->hy - bound * v->z;
    default: return bound * v->z - v->hy;
    }
}

static int before(const GeometryVertex *a, const GeometryVertex *b) {
    if (a->hx != b->hx) return a->hx < b->hx;
    if (a->hy != b->hy) return a->hy < b->hy;
    return a->z < b->z;
}

static GeometryVertex intersect(const GeometryVertex *a, const GeometryVertex *b,
                                 int plane, double bound) {
    /* Both directions of a shared edge use the same endpoint ordering,
     * giving identical intersections instead of a rounding gap. */
    if (before(b, a)) {
        const GeometryVertex *tmp = a; a = b; b = tmp;
    }
    double da = distance(a, plane, bound), db = distance(b, plane, bound);
    if (da == 0.0) return *a;
    if (db == 0.0) return *b;
    double t = da / (da - db);
    GeometryVertex o;
#define LERP(field) o.field = a->field + t * (b->field - a->field)
    LERP(hx); LERP(hy); LERP(z); LERP(u); LERP(v);
    LERP(r); LERP(g); LERP(b);
#undef LERP
    if (plane == 0) o.z = GEOMETRY_NEAR;
    else if (plane <= 2) o.hx = bound * o.z;
    else o.hy = bound * o.z;
    return o;
}

int geometry_clip_triangle(const GeometryVertex in[3],
                           double left, double top, double right, double bottom,
                           GeometryVertex out[GEOMETRY_CLIP_CAPACITY]) {
    if (!(left < right && top < bottom)) return 0;
    for (int i = 0; i < 3; i++) {
        const GeometryVertex *v = &in[i];
        if (!isfinite(v->hx) || !isfinite(v->hy) || !isfinite(v->z) ||
            !isfinite(v->u) || !isfinite(v->v) || !isfinite(v->r) ||
            !isfinite(v->g) || !isfinite(v->b)) return 0;
    }
    GeometryVertex buf[2][GEOMETRY_CLIP_CAPACITY];
    memcpy(buf[0], in, sizeof(*in) * 3);
    int n = 3, src = 0;
    const double bounds[5] = {GEOMETRY_NEAR, left, right, top, bottom};
    for (int plane = 0; plane < 5 && n; plane++) {
        int count = 0;
        GeometryVertex *a = buf[src], *b = buf[src ^ 1];
        for (int i = 0; i < n; i++) {
            const GeometryVertex *p = &a[(i + n - 1) % n], *q = &a[i];
            bool pin = distance(p, plane, bounds[plane]) >= 0.0;
            bool qin = distance(q, plane, bounds[plane]) >= 0.0;
            if (pin != qin) {
                if (count == GEOMETRY_CLIP_CAPACITY) return 0;
                b[count++] = intersect(p, q, plane, bounds[plane]);
            }
            if (qin) {
                if (count == GEOMETRY_CLIP_CAPACITY) return 0;
                b[count++] = *q;
            }
        }
        n = count; src ^= 1;
    }
    if (n < 3) return 0;
    memcpy(out, buf[src], sizeof(*out) * (size_t)n);
    return n;
}
