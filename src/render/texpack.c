/* Texture pack: replacements.ini and its PNG images in a folder.
 *
 *     # comment
 *     [any name]
 *     image = picture.png
 *     screen = x y w h                         (optional: one image for all sources)
 *     source = page_x page_y bits clut_x clut_y u v w h
 *
 * Sources are the columns of a texture dump's list. Without `screen`, each one
 * is a sprite replaced where it is drawn. */
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
    bool has_at;
    int group;
} Section;

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
static int load_image(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
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
    SDL_DestroySurface(rgba);
    return image;
}

/* the group is made on the first source, once image and screen have been read */
static int section_group(Section *sec, const char *dir) {
    if (sec->group >= 0 || !sec->image[0])
        return sec->group;
    int image = load_image(dir, sec->image);
    if (image >= 0)
        sec->group = texrep_add_group(image, sec->has_at ? sec->at : NULL);
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
        sec->has_at = sscanf(value, "%d %d %d %d", &sec->at[0], &sec->at[1], &sec->at[2], &sec->at[3]) == 4;
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
