/*
 * Start-up launcher: choose the disc image, change settings and controls,
 * then start the game. The same pages are shown in game as an overlay.
 * Dear ImGui on the SDL GPU API.
 */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_VERSION "0.1.1"

/* Runs the launcher in `win` until the player starts the game (returns
 * true) or quits (false). `cfg` holds the settings for this run and is
 * edited in place; every change is also applied to `saved` (the contents
 * of config.ini, without command-line overrides) and written to `path`.
 * On success cfg->disc holds the validated .cue path. */
bool launcher_run(SDL_Window *win, Settings *cfg, Settings *saved, const char *path);

/* Error dialog when no GPU API can be used (main.c) */
void platform_show_gpu_error(SDL_Window *win);

/* Testing/docs: render `page` ("main", "settings", "controls") to a PNG at
 * `path` instead of running interactively. Call before launcher_run. */
void launcher_debug_shot(const char *path, const char *page);
/* Testing: comma-separated SDL key names pressed one after another. */
void launcher_debug_keys(const char *list);

/* ---- In-game overlay ------------------------------------------------
 * The launcher's Settings and Controls pages plus a pause menu, drawn on
 * top of the running game with the game renderer's GPU device. The
 * frontend runs the loop: overlay_open, then per frame overlay_event for
 * each SDL event, overlay_update, and a presentation that calls
 * overlay_render; overlay_close when overlay_update stops returning
 * OVERLAY_OPEN. Settings edits go to `cfg` (applied live by the frontend)
 * and `saved`, which is written to `path`. */
typedef struct OverlayInfo {
    bool software;           /* software renderer: no PGXP / widescreen */
    /* values this run started with, for settings that need a restart */
    int running_scale;
    bool running_vsync;
    bool running_skip_intro;
} OverlayInfo;

enum { OVERLAY_OPEN, OVERLAY_RESUME, OVERLAY_QUIT };

bool overlay_open(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt,
                  Settings *cfg, Settings *saved, const char *path, const OverlayInfo *info);
void overlay_event(const SDL_Event *e);
int overlay_update(void);
void overlay_render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h);
void overlay_close(void);

#ifdef __cplusplus
}
#endif
