/*
 * Widescreen support.
 *
 * The game renders into a framebuffer that the PS1 displays at 4:3. For a
 * wider aspect ratio the GTE's horizontal perspective projection is scaled
 * by k = (4/3) / aspect, so the same framebuffer holds a wider field of
 * view, and the image is then displayed at the wider aspect.
 *
 * Primitives that were not produced by the GTE (HUD, text, menus, 2D
 * backgrounds) would come out stretched by 1/k; the renderer squashes them
 * back. To tell them apart, every screen point produced by RTPS/RTPT is
 * remembered for the current and the previous frame (the game builds frame
 * N+1 while the GPU draws frame N).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

extern bool g_ws_enabled;
extern float g_ws_aspect;   /* display aspect, e.g. 16/9 */
extern float g_ws_k;        /* horizontal projection scale (4/3) / aspect */
extern int32_t g_ws_k_fp;   /* g_ws_k in 16.16 fixed point */

/* The display is only widened while the game draws 3D: pure 2D screens
 * (loading screens, bitmaps, static menus) stay 4:3, pillarboxed. */
extern bool g_ws_wide_display;
extern uint32_t g_ws_3d_count;     /* 3D primitives drawn this frame */

void ws_configure(float aspect);   /* aspect <= 4/3 disables */
void ws_record(int32_t x, int32_t y);
bool ws_projected(int32_t x, int32_t y);  /* within +-1 pixel */
bool ws_projected_exact(int32_t x, int32_t y);
void ws_new_frame(void);
