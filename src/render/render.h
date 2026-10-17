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
