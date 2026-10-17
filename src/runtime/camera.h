/*
 * Camera input (native.c): mouse look and the right stick, fed straight
 * into the game's turn/look commands right before the player update reads
 * them, instead of through the original controller path (two frames of
 * delay, sample averaging and a 38% stick dead zone).
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse look not yet applied, in game angle units (4096 = a full turn):
 * x > 0 turns right, y > 0 looks down. The frontend adds to them each
 * vblank, the player update takes what it applies; g_cam_mouse_age counts
 * vblanks since then, so movement made while the game is not turning
 * (menus, pause) can be dropped instead of applied later in one go. */
extern float g_cam_mouse_x, g_cam_mouse_y;
extern int g_cam_mouse_age;

/* Right stick: when on, the camera axes are computed from the stick
 * position read at the last vblank, with this dead zone (percent of the
 * stick's travel), response curve exponent and speed (percent of the
 * game's turn/look speed at full deflection). */
extern bool g_cam_stick;
extern int g_cam_stick_dead_zone;
extern float g_cam_stick_curve;
extern int g_cam_stick_speed;

#ifdef __cplusplus
}
#endif
