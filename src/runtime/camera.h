/*
 * Camera and stick input (native.c). The original reads the analog sticks
 * two frames late, averages samples and ignores the first 38% of their
 * travel; mouse look that drives the right stick inherits all of that.
 * Here the mouse turns the camera directly, and both sticks can be read
 * as they are right now, with a smaller dead zone.
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

/* Responsive sticks: both sticks are read as set at the last vblank, with
 * this dead zone (percent of the stick's travel). The camera stick also
 * gets a response curve exponent and a speed (percent of the game's
 * turn/look speed at full deflection); movement stays linear. */
extern bool g_stick_direct;
extern int g_stick_dead_zone;
extern float g_cam_stick_curve;
extern int g_cam_stick_speed;

#ifdef __cplusplus
}
#endif
