/*
 * Texture replacement: 2D primitives the game draws from certain VRAM textures
 * are drawn from high resolution images instead (rules from a texture pack,
 * src/render/texpack.c, and the renderer's image_quad).
 *
 * A replacement takes the place of one sprite, or of a group of sprites that
 * together make one picture, such as the layers of the title logo: the group's
 * image goes at a fixed spot on the screen when the first of them is drawn, or
 * across the whole window for a band like those of the menus.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gpu.h"

extern bool g_texrep_on;

typedef enum {
    TEXREP_SPRITE,  /* each source is replaced where it is drawn */
    TEXREP_SCREEN,  /* the image at {x, y, w, h} in the buffer's pixels */
    TEXREP_FILL,    /* the image across the window at rows {y, h}, for sources drawn there */
} TexrepPlace;

/* a replacement; returns its id, or -1 if the tables are full. `at` is unused for
 * sprites; `aspect` is the image's width / height. */
int texrep_add_group(int image, float aspect, TexrepPlace place, const int *at);
/* source texture of a replacement, as listed by a texture dump: page position in
 * VRAM, bits per texel, palette position in VRAM, rectangle in the page */
bool texrep_add_source(int group, int page_x, int page_y, int bits, int clut_x, int clut_y, int u,
                       int v, int w, int h);

/* a textured quad, corners in the order the GPU takes them: true if it is
 * replaced and must not be drawn */
bool texrep_quad(const GpuState *s, const GVtx q[4], uint16_t tpage, uint16_t clut, uint32_t flags);
void texrep_new_frame(void);
