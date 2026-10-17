/*
 * Disc image selection for the launcher: resolve what the player picked
 * (cue sheet, track .bin or folder) to a cue sheet, check that it is the
 * supported game, and read title art from it.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Last path separator in p ('/', or '\\' as Windows also uses), or NULL. */
static inline const char *path_last_sep(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (!a)
        return b;
    if (!b)
        return a;
    return a > b ? a : b;
}

typedef enum {
    DISC_OK,
    DISC_NONE,        /* nothing selected */
    DISC_NOT_FOUND,   /* no cue sheet at/next to the path */
    DISC_UNREADABLE,  /* cue/track files missing or not a PS1 data disc */
    DISC_WRONG_GAME,  /* readable, but not Alien Resurrection (USA) */
} DiscStatus;

/* Resolve `path` to a cue sheet (written to cue) without opening it. */
bool disc_resolve(const char *path, char *cue, size_t n);

/* Resolve and open the disc, check SYSTEM.CNF for SLUS_006.33. On success
 * the disc stays open (for disc_title_art). `msg` gets a short description
 * for the player either way. */
DiscStatus disc_check(const char *path, char *cue, size_t cue_n, char *msg, size_t msg_n);

/* Title art from the open disc (a loading screen in PACK.BIN), as RGBA8.
 * Returns NULL if unavailable; free() the result. */
uint8_t *disc_title_art(int *w, int *h);

#ifdef __cplusplus
}
#endif
