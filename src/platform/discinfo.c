#include "discinfo.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../runtime/iso.h"
#include "../runtime/psx.h"

static bool has_ext(const char *name, const char *ext) {
    size_t l = strlen(name), e = strlen(ext);
    return l > e && !strcasecmp(name + l - e, ext);
}

/* Does the cue sheet at `cue` reference a track file called `bin`? */
static bool cue_mentions(const char *cue, const char *bin) {
    FILE *f = fopen(cue, "r");
    if (!f)
        return false;
    char line[1024];
    bool found = false;
    while (!found && fgets(line, sizeof line, f))
        found = strncmp(line, "FILE", 4) == 0 && strstr(line, bin) != NULL;
    fclose(f);
    return found;
}

/* First cue sheet in `dir` (referencing `bin` if given). */
static bool cue_in_dir(const char *dir, const char *bin, char *out, size_t n) {
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !has_ext(e->d_name, ".cue"))
            continue;
        snprintf(out, n, "%s/%s", dir, e->d_name);
        found = !bin || cue_mentions(out, bin);
    }
    closedir(d);
    return found;
}

static bool resolve_rel(const char *path, char *cue, size_t n);

/* Resolved cue path, made absolute so config.ini works from any folder. */
bool disc_resolve(const char *path, char *cue, size_t n) {
    char tmp[1024];
    if (!resolve_rel(path, tmp, sizeof tmp))
        return false;
    char abs[PATH_MAX];
    snprintf(cue, n, "%s", realpath(tmp, abs) ? abs : tmp);
    return true;
}

static bool resolve_rel(const char *path, char *cue, size_t n) {
    struct stat st;
    if (!path || !*path || stat(path, &st))
        return false;
    if (S_ISDIR(st.st_mode))
        return cue_in_dir(path, NULL, cue, n);
    if (has_ext(path, ".cue")) {
        snprintf(cue, n, "%s", path);
        return true;
    }
    /* a track file: find the cue sheet next to it that uses it */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    const char *base = slash ? slash + 1 : path;
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    char bin[256];
    snprintf(bin, sizeof bin, "%s", base);
    return cue_in_dir(dir, bin, cue, n);
}

DiscStatus disc_check(const char *path, char *cue, size_t cue_n, char *msg, size_t msg_n) {
    if (!path || !*path) {
        snprintf(msg, msg_n, "No disc image selected.");
        return DISC_NONE;
    }
    if (!disc_resolve(path, cue, cue_n)) {
        snprintf(msg, msg_n, "No .cue sheet found for the selected file.");
        return DISC_NOT_FOUND;
    }
    if (!disc_open(cue)) {
        snprintf(msg, msg_n, "Cannot read the disc image (missing track files?).");
        return DISC_UNREADABLE;
    }
    uint32_t lba, size;
    char cnf[2048 + 1];
    if (!iso_find("SYSTEM.CNF", &lba, &size) || size > 2048 || !iso_read(lba, (uint8_t *)cnf, size)) {
        disc_close();
        snprintf(msg, msg_n, "This is not a PlayStation game disc.");
        return DISC_UNREADABLE;
    }
    cnf[size] = 0;
    if (!strstr(cnf, "SLUS_006.33")) {
        char boot[64] = "?";
        const char *b = strstr(cnf, "cdrom:");
        if (b) {
            b += 6;
            while (*b == '\\')
                b++;
            size_t i = 0;
            while (b[i] && b[i] != ';' && b[i] != '\r' && b[i] != '\n' && i < sizeof boot - 1)
                boot[i] = b[i], i++;
            boot[i] = 0;
        }
        disc_close();
        snprintf(msg, msg_n, "Unsupported disc (%s). This port needs Alien Resurrection (USA), SLUS-00633.",
                 boot);
        return DISC_WRONG_GAME;
    }
    snprintf(msg, msg_n, "Alien Resurrection (USA) - SLUS-00633");
    return DISC_OK;
}

uint8_t *disc_title_art(int *w, int *h) {
    /* PACK.BIN: u32 count, then {char name[12]; u32 size; u32 offset} */
    uint32_t lba, size;
    if (!iso_find("PACK.BIN", &lba, &size))
        return NULL;
    uint8_t dir[4096];
    if (!iso_read(lba, dir, sizeof dir))
        return NULL;
    uint32_t count;
    memcpy(&count, dir, 4);
    if (count == 0 || 4 + count * 20 > sizeof dir)
        return NULL;
    uint32_t esize = 0, eoff = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = dir + 4 + i * 20;
        if (!strncmp((const char *)e, "ELOAD30.TIM", 12)) {
            memcpy(&esize, e + 12, 4);
            memcpy(&eoff, e + 16, 4);
            break;
        }
    }
    if (esize < 12 || esize > 1024 * 1024 || (eoff & 2047))
        return NULL;
    uint8_t *raw = malloc(esize + 2048);
    if (!raw || !iso_read(lba + eoff / 2048, raw, esize)) {
        free(raw);
        return NULL;
    }
    /* loading screen: u16 width, u16 height, 8 bytes, then 15-bit pixels */
    int iw = raw[0] | raw[1] << 8, ih = raw[2] | raw[3] << 8;
    if (iw <= 0 || ih <= 0 || 12 + (uint32_t)(iw * ih * 2) > esize) {
        free(raw);
        return NULL;
    }
    uint8_t *rgba = malloc((size_t)iw * (size_t)ih * 4);
    for (int i = 0; i < iw * ih; i++) {
        uint16_t p = (uint16_t)(raw[12 + i * 2] | raw[13 + i * 2] << 8);
        int r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
        rgba[i * 4 + 0] = (uint8_t)(r << 3 | r >> 2);
        rgba[i * 4 + 1] = (uint8_t)(g << 3 | g >> 2);
        rgba[i * 4 + 2] = (uint8_t)(b << 3 | b >> 2);
        rgba[i * 4 + 3] = 255;
    }
    free(raw);
    *w = iw;
    *h = ih;
    return rgba;
}
