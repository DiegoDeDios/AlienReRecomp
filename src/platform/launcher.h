/*
 * Start-up launcher: choose the disc image, change settings and controls,
 * then start the game. Dear ImGui on the SDL GPU API.
 */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_VERSION "0.1.0"

/* Runs the launcher in `win` until the player starts the game (returns
 * true) or quits (false). `cfg` holds the settings for this run and is
 * edited in place; every change is also applied to `saved` (the contents
 * of config.ini, without command-line overrides) and written to `path`.
 * On success cfg->disc holds the validated .cue path. */
bool launcher_run(SDL_Window *win, Settings *cfg, Settings *saved, const char *path);

/* Testing/docs: render `page` ("main", "settings", "controls") to a PNG at
 * `path` instead of running interactively. Call before launcher_run. */
void launcher_debug_shot(const char *path, const char *page);
/* Testing: comma-separated SDL key names pressed one after another. */
void launcher_debug_keys(const char *list);

#ifdef __cplusplus
}
#endif
