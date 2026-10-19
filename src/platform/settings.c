#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/psx.h"

const ActionInfo g_actions[ACT_COUNT] = {
    [ACT_MOVE_UP] = {"move_forward", "Move forward", "Left stick up", true},
    [ACT_MOVE_DOWN] = {"move_back", "Move back", "Left stick down", true},
    [ACT_MOVE_LEFT] = {"strafe_left", "Strafe left", "Left stick left", true},
    [ACT_MOVE_RIGHT] = {"strafe_right", "Strafe right", "Left stick right", true},
    [ACT_LOOK_UP] = {"look_up", "Look up", "Right stick up", true},
    [ACT_LOOK_DOWN] = {"look_down", "Look down", "Right stick down", true},
    [ACT_LOOK_LEFT] = {"turn_left", "Turn left", "Right stick left", true},
    [ACT_LOOK_RIGHT] = {"turn_right", "Turn right", "Right stick right", true},
    [ACT_DPAD_UP] = {"dpad_up", "Menu up", "D-pad up", false},
    [ACT_DPAD_DOWN] = {"dpad_down", "Menu down", "D-pad down", false},
    [ACT_DPAD_LEFT] = {"dpad_left", "Menu left", "D-pad left", false},
    [ACT_DPAD_RIGHT] = {"dpad_right", "Menu right", "D-pad right", false},
    [ACT_CROSS] = {"cross", "Accept / crouch", "Cross", false},
    [ACT_CIRCLE] = {"circle", "Use", "Circle", false},
    [ACT_SQUARE] = {"square", "Previous item", "Square", false},
    [ACT_TRIANGLE] = {"triangle", "Back / next item", "Triangle", false},
    [ACT_L1] = {"l1", "Next weapon", "L1", false},
    [ACT_L2] = {"l2", "Previous weapon", "L2", false},
    [ACT_R1] = {"r1", "Fire", "R1", false},
    [ACT_R2] = {"r2", "Action", "R2", false},
    [ACT_L3] = {"l3", "Centre view", "L3", false},
    [ACT_R3] = {"r3", "Quick turn", "R3", false},
    [ACT_START] = {"start", "Pause", "Start", false},
    [ACT_SELECT] = {"select", "Look hold", "Select", false},
    [ACT_SPRINT] = {"sprint", "Sprint", "", false},
    [ACT_FAST_FORWARD] = {"fast_forward", "Fast forward", "", false},
    [ACT_QUICK_SAVE] = {"quick_save", "Quick save", "", false},
    [ACT_QUICK_LOAD] = {"quick_load", "Quick load", "", false},
};

static const uint16_t g_act_button[ACT_COUNT] = {
    [ACT_DPAD_UP] = PAD_UP, [ACT_DPAD_DOWN] = PAD_DOWN,
    [ACT_DPAD_LEFT] = PAD_LEFT, [ACT_DPAD_RIGHT] = PAD_RIGHT,
    [ACT_CROSS] = PAD_CROSS, [ACT_CIRCLE] = PAD_CIRCLE,
    [ACT_SQUARE] = PAD_SQUARE, [ACT_TRIANGLE] = PAD_TRIANGLE,
    [ACT_L1] = PAD_L1, [ACT_L2] = PAD_L2, [ACT_R1] = PAD_R1, [ACT_R2] = PAD_R2,
    [ACT_L3] = PAD_L3, [ACT_R3] = PAD_R3,
    [ACT_START] = PAD_START, [ACT_SELECT] = PAD_SELECT,
};

/* ------------------------------------------------------------------ */
/* defaults                                                            */

void settings_default_bindings(Settings *s) {
    static const struct { Action a; int k0, k1, pad; } d[] = {
        {ACT_MOVE_UP, SDL_SCANCODE_W, 0, -1},
        {ACT_MOVE_DOWN, SDL_SCANCODE_S, 0, -1},
        {ACT_MOVE_LEFT, SDL_SCANCODE_A, 0, -1},
        {ACT_MOVE_RIGHT, SDL_SCANCODE_D, 0, -1},
        {ACT_LOOK_UP, 0, 0, -1},
        {ACT_LOOK_DOWN, 0, 0, -1},
        {ACT_LOOK_LEFT, 0, 0, -1},
        {ACT_LOOK_RIGHT, 0, 0, -1},
        {ACT_DPAD_UP, SDL_SCANCODE_UP, 0, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {ACT_DPAD_DOWN, SDL_SCANCODE_DOWN, 0, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ACT_DPAD_LEFT, SDL_SCANCODE_LEFT, 0, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {ACT_DPAD_RIGHT, SDL_SCANCODE_RIGHT, 0, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ACT_CROSS, SDL_SCANCODE_SPACE, SDL_SCANCODE_C, SDL_GAMEPAD_BUTTON_SOUTH},
        {ACT_CIRCLE, SDL_SCANCODE_E, 0, SDL_GAMEPAD_BUTTON_EAST},
        {ACT_SQUARE, SDL_SCANCODE_Z, 0, SDL_GAMEPAD_BUTTON_WEST},
        {ACT_TRIANGLE, SDL_SCANCODE_X, SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_NORTH},
        {ACT_L1, SDL_SCANCODE_R, 0, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {ACT_L2, SDL_SCANCODE_Q, 0, PAD_TRIGGER_L},
        {ACT_R1, SDL_SCANCODE_LCTRL, BIND_MOUSE + SDL_BUTTON_LEFT, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {ACT_R2, SDL_SCANCODE_F, BIND_MOUSE + SDL_BUTTON_RIGHT, PAD_TRIGGER_R},
        {ACT_L3, SDL_SCANCODE_V, 0, -1},
        {ACT_R3, SDL_SCANCODE_B, BIND_MOUSE + SDL_BUTTON_MIDDLE, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {ACT_START, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START},
        {ACT_SELECT, SDL_SCANCODE_TAB, 0, SDL_GAMEPAD_BUTTON_BACK},
        {ACT_SPRINT, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {ACT_FAST_FORWARD, SDL_SCANCODE_GRAVE, 0, -1},
        {ACT_QUICK_SAVE, SDL_SCANCODE_F5, 0, -1},
        {ACT_QUICK_LOAD, SDL_SCANCODE_F9, 0, -1},
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        s->key[d[i].a][0] = d[i].k0;
        s->key[d[i].a][1] = d[i].k1;
        s->pad[d[i].a] = d[i].pad;
    }
}

const EnemyGroupInfo g_enemy_groups[DMG_GROUP_COUNT] = {
    [DMG_ALIENS] = {"alien_warriors", "Alien warriors",
                    "Claws and bites of the adult aliens."},
    [DMG_FACEHUGGERS] = {"facehuggers", "Facehuggers", "Facehugger attacks."},
    [DMG_CHESTBURSTERS] = {"chestbursters", "Chestbursters", "Chestburster attacks."},
    [DMG_QUEEN] = {"queen", "Queen", "The alien queen."},
    [DMG_NEWBORN] = {"newborn", "Newborn", "The Newborn."},
    [DMG_MARINES] = {"marines", "Marines and soldiers",
                     "Gunfire and attacks from marines and other human enemies."},
    [DMG_SENTRY_TANK] = {"sentry_tank", "Sentry tank", "The sentry tank."},
    [DMG_ACID] = {"acid_blood", "Acid blood",
                  "Acid that sprays from aliens when they are hit close to you."},
};

const struct StickCurve g_stick_curves[STICK_CURVES] = {
    {"Linear", "linear", 1.0f},
    {"Smooth", "smooth", 1.5f},
    {"Precise", "precise", 2.2f},
};

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof *s);
    s->skip_intro = false;
    s->widescreen = true;
    s->aspect_w = 16, s->aspect_h = 9;
    s->internal_scale = 4;
    s->pgxp = true;
    s->vsync = true;
    snprintf(s->gpu_driver, sizeof s->gpu_driver, "auto");
    s->smooth = true;
    s->sprint = true;
    s->sprint_pct = 190;
    s->infinite_flashlight = true;
    s->quick_save = true;
    s->cpu_pct = 300;
    s->enemy_damage_pct = 100;
    for (int g = 0; g < DMG_GROUP_COUNT; g++)
        s->enemy_damage_group_pct[g] = 100;
    s->mouse_look = true;
    s->mouse_sens = 6.0f;
    s->stick_direct = true;
    s->stick_speed = 150;
    s->stick_dead_zone = 10;
    s->stick_curve = 1;
    settings_default_bindings(s);
}

const char *settings_path(void) {
    static char path[1100];
    if (!path[0])
        snprintf(path, sizeof path, "%s/config.ini", platform_data_dir());
    return path;
}

/* ------------------------------------------------------------------ */
/* binding names                                                       */

static const char *const g_mouse_names[] = {"", "Mouse Left", "Mouse Middle",
                                            "Mouse Right", "Mouse X1", "Mouse X2"};

const char *bind_key_name(int code, char *buf, size_t n) {
    if (code >= BIND_MOUSE && code <= BIND_MOUSE + 5)
        snprintf(buf, n, "%s", g_mouse_names[code - BIND_MOUSE]);
    else if (code > 0 && code < SDL_SCANCODE_COUNT && *SDL_GetScancodeName((SDL_Scancode)code))
        snprintf(buf, n, "%s", SDL_GetScancodeName((SDL_Scancode)code));
    else
        snprintf(buf, n, "%s", "");
    return buf;
}

static int key_from_name(const char *name) {
    for (int i = 1; i <= 5; i++)
        if (!SDL_strcasecmp(name, g_mouse_names[i]))
            return BIND_MOUSE + i;
    if (!*name || !SDL_strcasecmp(name, "none"))
        return 0;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN)
        return (int)sc;
    /* names written by SDL_GetScancodeName can be platform-specific
     * ("Left Option" on macOS) and not accepted by the reverse lookup */
    for (int i = 1; i < SDL_SCANCODE_COUNT; i++) {
        const char *n = SDL_GetScancodeName((SDL_Scancode)i);
        if (*n && !SDL_strcasecmp(n, name))
            return i;
    }
    static const struct { const char *from, *to; } alias[] = {
        {"Left Option", "Left Alt"}, {"Right Option", "Right Alt"},
        {"Left Command", "Left GUI"}, {"Right Command", "Right GUI"},
    };
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (!SDL_strcasecmp(name, alias[i].from))
            return (int)SDL_GetScancodeFromName(alias[i].to);
    return 0;
}

const char *bind_pad_name(int code, SDL_GamepadType type, char *buf, size_t n) {
    bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
              type == SDL_GAMEPAD_TYPE_PS5;
    const char *s = "";
    switch (code) {
    case PAD_TRIGGER_L: s = ps ? "L2" : "LT"; break;
    case PAD_TRIGGER_R: s = ps ? "R2" : "RT"; break;
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH: {
        if (type == SDL_GAMEPAD_TYPE_UNKNOWN || type == SDL_GAMEPAD_TYPE_STANDARD)
            type = SDL_GAMEPAD_TYPE_XBOXONE;
        static const char *const lbl[] = {"?", "A", "B", "X", "Y",
                                          "Cross", "Circle", "Square", "Triangle"};
        SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabelForType(type, (SDL_GamepadButton)code);
        s = (int)l >= 0 && (size_t)l < sizeof lbl / sizeof lbl[0] ? lbl[l] : "?";
        break;
    }
    case SDL_GAMEPAD_BUTTON_BACK: s = ps ? "Select / Share" : "View"; break;
    case SDL_GAMEPAD_BUTTON_GUIDE: s = "Home"; break;
    case SDL_GAMEPAD_BUTTON_START: s = ps ? "Start / Options" : "Menu"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: s = ps ? "L3" : "Left stick click"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: s = ps ? "R3" : "Right stick click"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: s = ps ? "L1" : "LB"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: s = ps ? "R1" : "RB"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: s = "D-pad up"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: s = "D-pad down"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: s = "D-pad left"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s = "D-pad right"; break;
    case SDL_GAMEPAD_BUTTON_MISC1: s = "Misc"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: s = "Paddle R1"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: s = "Paddle L1"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: s = "Paddle R2"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: s = "Paddle L2"; break;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: s = "Touchpad"; break;
    default: break;
    }
    snprintf(buf, n, "%s", s);
    return buf;
}

static const char *pad_id(int code) {
    if (code == PAD_TRIGGER_L) return "lefttrigger";
    if (code == PAD_TRIGGER_R) return "righttrigger";
    if (code >= 0 && code < SDL_GAMEPAD_BUTTON_COUNT) {
        const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
        if (s)
            return s;
    }
    return "none";
}

static int pad_from_id(const char *s) {
    if (!SDL_strcasecmp(s, "lefttrigger")) return PAD_TRIGGER_L;
    if (!SDL_strcasecmp(s, "righttrigger")) return PAD_TRIGGER_R;
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s);
    return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
}

/* ------------------------------------------------------------------ */
/* config.ini                                                          */

static bool parse_bool(const char *v) {
    return !SDL_strcasecmp(v, "true") || !SDL_strcasecmp(v, "on") ||
           !SDL_strcasecmp(v, "yes") || !strcmp(v, "1");
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    memmove(s, s + i, strlen(s + i) + 1);
}

static int action_by_id(const char *id) {
    for (int a = 0; a < ACT_COUNT; a++)
        if (!strcmp(g_actions[a].id, id))
            return a;
    return -1;
}

bool settings_load(Settings *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[1300], sect[32] = "";
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e)
                *e = 0;
            snprintf(sect, sizeof sect, "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(sect, "game")) {
            if (!strcmp(k, "disc")) snprintf(s->disc, sizeof s->disc, "%s", v);
            else if (!strcmp(k, "skip_intro")) s->skip_intro = parse_bool(v);
        } else if (!strcmp(sect, "video")) {
            if (!strcmp(k, "widescreen")) s->widescreen = parse_bool(v);
            else if (!strcmp(k, "aspect")) {
                int w, h;
                if (sscanf(v, "%d:%d", &w, &h) == 2 && w > 0 && h > 0)
                    s->aspect_w = w, s->aspect_h = h;
            } else if (!strcmp(k, "internal_scale")) s->internal_scale = clampi(atoi(v), 1, 8);
            else if (!strcmp(k, "pgxp")) s->pgxp = parse_bool(v);
            else if (!strcmp(k, "vsync")) s->vsync = parse_bool(v);
            else if (!strcmp(k, "smooth")) s->smooth = parse_bool(v);
            else if (!strcmp(k, "fullscreen")) s->fullscreen = parse_bool(v);
            else if (!strcmp(k, "show_fps")) s->show_fps = parse_bool(v);
            else if (!strcmp(k, "gpu_driver") &&
                     (!strcmp(v, "auto") || !strcmp(v, "vulkan") || !strcmp(v, "direct3d12")))
                snprintf(s->gpu_driver, sizeof s->gpu_driver, "%s", v);
        } else if (!strcmp(sect, "gameplay")) {
            if (!strcmp(k, "sprint")) s->sprint = parse_bool(v);
            else if (!strcmp(k, "sprint_speed")) s->sprint_pct = clampi(atoi(v), 100, 300);
            else if (!strcmp(k, "infinite_flashlight")) s->infinite_flashlight = parse_bool(v);
            else if (!strcmp(k, "quick_save")) s->quick_save = parse_bool(v);
            else if (!strcmp(k, "cpu_speed")) s->cpu_pct = clampi(atoi(v), 50, 800);
        } else if (!strcmp(sect, "enemy_damage")) {
            int pct = clampi(atoi(v), 0, ENEMY_DAMAGE_MAX);
            if (!strcmp(k, "all"))
                s->enemy_damage_pct = pct;
            for (int g = 0; g < DMG_GROUP_COUNT; g++)
                if (!strcmp(k, g_enemy_groups[g].id))
                    s->enemy_damage_group_pct[g] = pct;
        } else if (!strcmp(sect, "mouse")) {
            if (!strcmp(k, "mouse_look")) s->mouse_look = parse_bool(v);
            else if (!strcmp(k, "sensitivity")) s->mouse_sens = (float)atof(v);
        } else if (!strcmp(sect, "camera")) {
            /* "responsive_stick" (camera only) from the first version */
            if (!strcmp(k, "responsive_sticks") || !strcmp(k, "responsive_stick"))
                s->stick_direct = parse_bool(v);
            else if (!strcmp(k, "stick_speed")) s->stick_speed = clampi(atoi(v), 50, 300);
            else if (!strcmp(k, "stick_dead_zone")) s->stick_dead_zone = clampi(atoi(v), 0, 40);
            else if (!strcmp(k, "stick_curve")) {
                for (int i = 0; i < STICK_CURVES; i++)
                    if (!strcmp(v, g_stick_curves[i].id))
                        s->stick_curve = i;
            }
        } else if (!strcmp(sect, "keyboard")) {
            /* "action" = first slot, "action.2" = second slot; actions
             * not listed keep their default bindings */
            char *dot = strchr(k, '.');
            int slot = dot && dot[1] == '2' ? 1 : 0;
            if (dot)
                *dot = 0;
            int a = action_by_id(k);
            if (a >= 0)
                s->key[a][slot] = key_from_name(v);
        } else if (!strcmp(sect, "controller")) {
            int a = action_by_id(k);
            if (a >= 0 && !g_actions[a].stick)
                s->pad[a] = pad_from_id(v);
        }
    }
    fclose(f);
    if (s->mouse_sens < 0.5f) s->mouse_sens = 0.5f;
    if (s->mouse_sens > 30.0f) s->mouse_sens = 30.0f;
    return true;
}

bool settings_save(const Settings *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    const char *tf[2] = {"false", "true"};
    fprintf(f, "# Alien Resurrection native port settings (written by the launcher)\n\n");
    fprintf(f, "[game]\ndisc = %s\nskip_intro = %s\n\n", s->disc, tf[s->skip_intro]);
    fprintf(f, "[video]\nwidescreen = %s\naspect = %d:%d\ninternal_scale = %d\n"
               "pgxp = %s\nvsync = %s\nsmooth = %s\nfullscreen = %s\nshow_fps = %s\n"
               "gpu_driver = %s\n\n",
            tf[s->widescreen], s->aspect_w, s->aspect_h, s->internal_scale,
            tf[s->pgxp], tf[s->vsync], tf[s->smooth], tf[s->fullscreen], tf[s->show_fps],
            s->gpu_driver);
    fprintf(f, "[gameplay]\nsprint = %s\nsprint_speed = %d\ninfinite_flashlight = %s\n"
               "quick_save = %s\ncpu_speed = %d\n\n",
            tf[s->sprint], s->sprint_pct, tf[s->infinite_flashlight], tf[s->quick_save],
            s->cpu_pct);
    fprintf(f, "# damage the player takes, percent of the original (all x group)\n"
               "[enemy_damage]\nall = %d\n", s->enemy_damage_pct);
    for (int g = 0; g < DMG_GROUP_COUNT; g++)
        fprintf(f, "%s = %d\n", g_enemy_groups[g].id, s->enemy_damage_group_pct[g]);
    fprintf(f, "\n");
    fprintf(f, "[mouse]\nmouse_look = %s\nsensitivity = %.2f\n\n", tf[s->mouse_look],
            (double)s->mouse_sens);
    fprintf(f, "[camera]\nresponsive_sticks = %s\nstick_speed = %d\nstick_dead_zone = %d\n"
               "stick_curve = %s\n\n",
            tf[s->stick_direct], s->stick_speed, s->stick_dead_zone,
            g_stick_curves[s->stick_curve].id);
    fprintf(f, "[keyboard]\n");
    char name[64];
    for (int a = 0; a < ACT_COUNT; a++)
        for (int k = 0; k < KEY_SLOTS; k++) {
            const char *n = bind_key_name(s->key[a][k], name, sizeof name);
            fprintf(f, "%s%s = %s\n", g_actions[a].id, k ? ".2" : "", *n ? n : "none");
        }
    fprintf(f, "\n[controller]\n");
    for (int a = 0; a < ACT_COUNT; a++)
        if (!g_actions[a].stick)
            fprintf(f, "%s = %s\n", g_actions[a].id, pad_id(s->pad[a]));
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* input                                                               */

static uint8_t axis8(int16_t v) {
    int a = (v + 32768) >> 8;
    return (uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
}

static bool pad_held(SDL_Gamepad *pad, int code) {
    if (!pad || code < 0)
        return false;
    if (code == PAD_TRIGGER_L)
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
    if (code == PAD_TRIGGER_R)
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    return code < SDL_GAMEPAD_BUTTON_COUNT && SDL_GetGamepadButton(pad, (SDL_GamepadButton)code);
}

bool input_pad_held(SDL_Gamepad *pad, int code) { return pad_held(pad, code); }

static bool key_held(const bool *kb, uint32_t mouse_buttons, int code) {
    if (code >= BIND_MOUSE)
        return (mouse_buttons & SDL_BUTTON_MASK(code - BIND_MOUSE)) != 0;
    return code > 0 && code < SDL_SCANCODE_COUNT && kb[code];
}

void input_read(const Settings *s, SDL_Gamepad *pad, uint32_t mouse_buttons, InputState *out) {
    const bool *kb = SDL_GetKeyboardState(NULL);
    bool held[ACT_COUNT];
    for (int a = 0; a < ACT_COUNT; a++) {
        held[a] = false;
        for (int k = 0; k < KEY_SLOTS; k++)
            held[a] |= key_held(kb, mouse_buttons, s->key[a][k]);
    }
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 0x80;
    for (int a = 0; a < ACT_COUNT; a++)
        if (held[a] || pad_held(pad, s->pad[a]))
            out->buttons |= g_act_button[a];
    if (held[ACT_MOVE_UP]) out->ly = 0x00;
    if (held[ACT_MOVE_DOWN]) out->ly = 0xFF;
    if (held[ACT_MOVE_LEFT]) out->lx = 0x00;
    if (held[ACT_MOVE_RIGHT]) out->lx = 0xFF;
    if (held[ACT_LOOK_UP]) out->ry = 0x00;
    if (held[ACT_LOOK_DOWN]) out->ry = 0xFF;
    if (held[ACT_LOOK_LEFT]) out->rx = 0x00;
    if (held[ACT_LOOK_RIGHT]) out->rx = 0xFF;
    out->sprint = held[ACT_SPRINT];
    out->fast_forward = held[ACT_FAST_FORWARD] || pad_held(pad, s->pad[ACT_FAST_FORWARD]);
    out->quick_save = held[ACT_QUICK_SAVE] || pad_held(pad, s->pad[ACT_QUICK_SAVE]);
    out->quick_load = held[ACT_QUICK_LOAD] || pad_held(pad, s->pad[ACT_QUICK_LOAD]);

    static bool stick_sprint;
    if (!pad) {
        stick_sprint = false;
        return;
    }
    /* sticks override the keyboard only when deflected */
    const int dz = 6000;
    int16_t ax[4] = {SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)};
    if (abs(ax[0]) > dz) out->lx = axis8(ax[0]);
    if (abs(ax[1]) > dz) out->ly = axis8(ax[1]);
    if (abs(ax[2]) > dz) out->rx = axis8(ax[2]);
    if (abs(ax[3]) > dz) out->ry = axis8(ax[3]);
    /* Sprint on a stick click latches until the left stick returns to
     * centre, so the stick doesn't have to stay pressed in while steering;
     * on any other button it is held. */
    int sp = s->pad[ACT_SPRINT];
    if (sp == SDL_GAMEPAD_BUTTON_LEFT_STICK || sp == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
        if (pad_held(pad, sp))
            stick_sprint = true;
        else if (abs(ax[0]) <= dz && abs(ax[1]) <= dz)
            stick_sprint = false;
        out->sprint |= stick_sprint;
    } else {
        out->sprint |= pad_held(pad, sp);
    }
}
