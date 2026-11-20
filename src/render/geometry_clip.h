#pragma once
#include <stdbool.h>

/* Screen numerators and depth, with attributes linear in camera space.
 * Project only after all clipping planes have been applied. */
typedef struct GeometryVertex {
    double hx, hy, z;
    double u, v, r, g, b;
} GeometryVertex;

/* A triangle clipped by five planes has at most eight vertices. */
#define GEOMETRY_CLIP_CAPACITY 12
#define GEOMETRY_NEAR 1.0

int geometry_clip_triangle(const GeometryVertex in[3],
                           double left, double top, double right, double bottom,
                           GeometryVertex out[GEOMETRY_CLIP_CAPACITY]);
