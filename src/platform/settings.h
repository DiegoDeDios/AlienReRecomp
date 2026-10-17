/*
 * User settings (config.ini in the app data folder) and input bindings.
 * Edited by the launcher, applied by the frontend when the game starts.
 */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "../runtime/enemy_damage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the player can bind. Stick directions are digital on the
 * keyboard; a controller's sticks always drive the analog sticks. */
typedef enum {
    ACT_MOVE_UP, ACT_MOVE_DOWN, ACT_MOVE_LEFT, ACT_MOVE_RIGHT,
    ACT_LOOK_UP, ACT_LOOK_DOWN, ACT_LOOK_LEFT, ACT_LOOK_RIGHT,
    ACT_DPAD_UP, ACT_DPAD_DOWN, ACT_DPAD_LEFT, ACT_DPAD_RIGHT,
    ACT_CROSS, ACT_CIRCLE, ACT_SQUARE, ACT_TRIANGLE,
    ACT_L1, ACT_L2, ACT_R1, ACT_R2, ACT_L3, ACT_R3,
    ACT_START, ACT_SELECT,
    ACT_SPRINT, ACT_FAST_FORWARD,
    ACT_COUNT
} Action;

typedef struct ActionInfo {
    const char *id;      /* config key */
    const char *label;   /* what it does in the game */
    const char *pad;     /* PS1 control it emulates ("" = port feature) */
    bool stick;          /* driven by a controller stick (no button binding) */
} ActionInfo;
extern const ActionInfo g_actions[ACT_COUNT];

/* Keyboard/mouse binding codes: 0 = none, 1..511 SDL_Scancode,
 * BIND_MOUSE + n = mouse button n (SDL_BUTTON_LEFT = 1 ...). */
#define BIND_MOUSE 0x1000
#define KEY_SLOTS 2
/* Controller binding codes: -1 = none, 0.. SDL_GamepadButton,
 * PAD_TRIGGER_L / PAD_TRIGGER_R = analog triggers. */
#define PAD_TRIGGER_L 100
#define PAD_TRIGGER_R 101

typedef struct Settings {
    char disc[1024];         /* .cue sheet of the player's disc image */
    bool skip_intro;         /* start ALIEN.BIN directly (no logos/FMV) */
    /* video */
    bool widescreen;
    int aspect_w, aspect_h;  /* widescreen aspect, e.g. 16:9 */
    int internal_scale;      /* GPU internal resolution multiplier */
    bool pgxp;
    bool vsync;
    bool smooth;
    bool fullscreen;
    bool show_fps;
    char gpu_driver[16];     /* Windows: "auto", "vulkan" or "direct3d12" */
    /* gameplay */
    bool sprint;
    int sprint_pct;
    bool infinite_flashlight;  /* the flashlight battery never runs down */
    int cpu_pct;             /* emulated CPU speed (frame rate headroom) */
    /* damage the player takes from enemies, percent of the original */
    int enemy_damage_pct;                        /* all enemies */
    int enemy_damage_group_pct[DMG_GROUP_COUNT]; /* per kind of enemy */
    /* input */
    bool mouse_look;
    float mouse_sens;
    /* sticks: both read directly (no delay, own dead zone); speed and
     * curve apply to the camera stick */
    bool stick_direct;
    int stick_speed;         /* camera: percent of the game's turn/look speed */
    int stick_dead_zone;     /* both sticks: percent of the stick's travel */
    int stick_curve;         /* camera: index into g_stick_curves */
    int key[ACT_COUNT][KEY_SLOTS];
    int pad[ACT_COUNT];
} Settings;

void settings_defaults(Settings *s);
void settings_default_bindings(Settings *s);
bool settings_load(Settings *s, const char *path);
bool settings_save(const Settings *s, const char *path);
const char *settings_path(void);

/* Kinds of enemy with their own damage setting (DMG_*). */
typedef struct EnemyGroupInfo {
    const char *id;     /* config.ini key in [enemy_damage] */
    const char *label;  /* shown in Settings */
    const char *desc;
} EnemyGroupInfo;
extern const EnemyGroupInfo g_enemy_groups[DMG_GROUP_COUNT];
#define ENEMY_DAMAGE_MAX 200

/* right stick response curves: name and exponent */
#define STICK_CURVES 3
extern const struct StickCurve { const char *name, *id; float exponent; } g_stick_curves[STICK_CURVES];

/* binding names (shown in the launcher, stored in config.ini) */
const char *bind_key_name(int code, char *buf, size_t n);
const char *bind_pad_name(int code, SDL_GamepadType type, char *buf, size_t n);

/* Current pad state from keyboard, mouse buttons and controller. */
typedef struct InputState {
    uint16_t buttons;            /* PAD_* bits */
    uint8_t lx, ly, rx, ry;      /* analog sticks, 0x80 = centre */
    bool sprint, fast_forward;
} InputState;
void input_read(const Settings *s, SDL_Gamepad *pad, uint32_t mouse_buttons,
                InputState *out);
/* Is controller binding `code` (button or PAD_TRIGGER_*) held on `pad`? */
bool input_pad_held(SDL_Gamepad *pad, int code);

#ifdef __cplusplus
}
#endif
