/*
 * Presentation and hardware rendering (SDL3 GPU API: Metal on macOS,
 * Vulkan / D3D12 elsewhere).
 */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct RenderConfig {
    int scale;          /* internal resolution multiplier (1 = native) */
    bool software;      /* use the software rasterizer, GPU only presents */
    bool headless;      /* no window: render offscreen (screenshots) */
    bool smooth;        /* linear filtering when scaling to the window */
    bool vsync;         /* present with vsync (no tearing) */
    bool vsync_paced;   /* one frame in flight: vsync paces emulation */
} RenderConfig;

bool render_init(SDL_Window *win, const RenderConfig *cfg);
void render_shutdown(void);
/* Draw the current display area to the window (or offscreen target). */
void render_present(void);
/* Read back the last presented display image as RGB24 (caller frees). */
bool render_capture(uint8_t **rgb, int *w, int *h);
const char *render_device_name(void);

/* In-game overlay: drawn on top of the game image in every window
 * presentation (cmd has no pass open; target holds the game image). */
typedef void (*RenderOverlayFn)(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target,
                                int w, int h);
void render_set_overlay(RenderOverlayFn fn);
SDL_GPUDevice *render_gpu_device(void);
SDL_GPUTextureFormat render_window_format(void);
/* Read back the window contents (game image + overlay) at the window's
 * pixel size as RGB24 (caller frees). */
bool render_capture_window(uint8_t **rgb, int *w, int *h);
/* Settings that can change while the game runs. */
void render_set_smooth(bool smooth);
/* Keep showing the current frame at its current aspect ratio (paused game
 * under the overlay) even if the widescreen settings change meanwhile. */
void render_hold_display_aspect(bool hold);
