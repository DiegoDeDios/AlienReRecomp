/* Texture pack: replacements.ini and its PNG images in a folder.
 *
 *     # comment
 *     [any name]
 *     image = picture.png
 *     screen = x y w h                         (optional: one image for all sources)
 *     fill = y h                               (or: across the window, at these rows)
 *     source = page_x page_y bits clut_x clut_y u v w h
 *
 * Sources are the columns of a texture dump's list. Without `screen` or `fill`,
 * each one is a sprite replaced where it is drawn. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hw/texrep.h"
#include "../runtime/psx.h"
#include "render.h"

/* image of the current section, added when the section's rules are known */
typedef struct Section {
    char image[256];
    int at[4];
    TexrepPlace place;
    int group;
} Section;

/* images already loaded: two sections can share one */
#define MAX_LOADED 64
static struct {
    char path[1024];
    int image;
    float aspect;
} g_loaded[MAX_LOADED];
static int g_nloaded;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && strchr(" \t\r\n", e[-1]))
        *--e = 0;
    return s;
}

/* decoded with alpha premultiplied: filtering then does not pull in the colour
 * of transparent pixels */
static int load_image(const char *dir, const char *name, float *aspect) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int i = 0; i < g_nloaded; i++)
        if (!strcmp(g_loaded[i].path, path)) {
            *aspect = g_loaded[i].aspect;
            return g_loaded[i].image;
        }
    SDL_Surface *png = SDL_LoadPNG(path);
    if (!png) {
        LOGW("textures", "%s: %s", path, SDL_GetError());
        return -1;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(png);
    if (!rgba)
        return -1;
    for (int y = 0; y < rgba->h; y++) {
        uint8_t *p = (uint8_t *)rgba->pixels + y * rgba->pitch;
        for (int x = 0; x < rgba->w; x++, p += 4)
            for (int c = 0; c < 3; c++)
                p[c] = (uint8_t)(p[c] * p[3] / 255);
    }
    int image = render_add_image(rgba->pixels, rgba->w, rgba->h, rgba->pitch);
    *aspect = (float)rgba->w / (float)rgba->h;
    SDL_DestroySurface(rgba);
    if (image >= 0 && g_nloaded < MAX_LOADED) {
        snprintf(g_loaded[g_nloaded].path, sizeof g_loaded[g_nloaded].path, "%s", path);
        g_loaded[g_nloaded].image = image;
        g_loaded[g_nloaded++].aspect = *aspect;
    }
    return image;
}

/* the group is made on the first source, once image and screen have been read */
static int section_group(Section *sec, const char *dir) {
    if (sec->group >= 0 || !sec->image[0])
        return sec->group;
    float aspect = 1.0f;
    int image = load_image(dir, sec->image, &aspect);
    if (image >= 0)
        sec->group = texrep_add_group(image, aspect, sec->place, sec->at);
    sec->image[0] = 0;
    return sec->group;
}

static void read_line(char *line, Section *sec, const char *dir) {
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';')
        return;
    if (*s == '[') {
        *sec = (Section){.group = -1};
        return;
    }
    char *eq = strchr(s, '=');
    if (!eq)
        return;
    *eq = 0;
    const char *key = trim(s);
    char *value = trim(eq + 1);
    if (!strcmp(key, "image")) {
        snprintf(sec->image, sizeof sec->image, "%s", value);
    } else if (!strcmp(key, "screen")) {
        if (sscanf(value, "%d %d %d %d", &sec->at[0], &sec->at[1], &sec->at[2], &sec->at[3]) == 4)
            sec->place = TEXREP_SCREEN;
    } else if (!strcmp(key, "fill")) {
        if (sscanf(value, "%d %d", &sec->at[0], &sec->at[1]) == 2)
            sec->place = TEXREP_FILL;
    } else if (!strcmp(key, "source")) {
        int f[9];
        if (sscanf(value, "%d %d %d %d %d %d %d %d %d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6],
                   &f[7], &f[8]) == 9)
            texrep_add_source(section_group(sec, dir), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
    }
}

void render_load_texture_pack(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/replacements.ini", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    Section sec = {.group = -1};
    char line[512];
    while (fgets(line, sizeof line, f))
        read_line(line, &sec, dir);
    fclose(f);
    LOGI("textures", "loaded %s", dir);
}
