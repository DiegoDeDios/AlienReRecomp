/*
 * Native frontend (SDL3): window/presentation, input, audio, frame pacing,
 * and the vblank driver that ties the emulated hardware timing to the host.
 *
 * Usage: alien [options]
 *   --cue PATH       disc image cue sheet
 *   --exe NAME       start executable (default SLUS_006.33; ALIEN.BIN skips
 *                    the intro movies)
 *   --headless       no window/audio
 *   --frames N       stop after N frames
 *   --shot N:PATH    write a PNG of the display at frame N (repeatable)
 *   --vram-shot N:PATH  write the whole VRAM at frame N
 *   --log LEVEL      0..3
 *   --press F:BUTTON[:LEN]  scripted input (BUTTON = start/cross/.../sprint)
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> /* Windows: SDL provides WinMain and calls main */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../hw/gpu.h"
#include "../render/render.h"
#include "../runtime/camera.h"
#include "../runtime/psx.h"
#include "../runtime/pgxp.h"
#include "../runtime/widescreen.h"
#include "discinfo.h"
#include "launcher.h"
#include "settings.h"

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);

/* NTSC timing: 263 lines of 3413 GPU clocks at 53.693175 MHz */
#define FRAME_CYCLES 566107
#define FRAME_NS (1000000000.0 * 263.0 * 3413.0 / 53693175.0)

static struct {
    bool headless;
    long max_frames;
    const char *cue;
    const char *exe;
    struct { long frame; char path[512]; bool vram; } shots[64];
    int nshots;
    struct { long start, count, step; char prefix[512]; } record;
    struct { long frame; char path[512]; } dumps[16];
    int ndumps;
    struct { long frame; uint32_t addr, value; int size; } pokes[32];
    int npokes;
    struct { long frame; long len; uint16_t button; bool sprint; } presses[256];
    int npresses;
    struct { long frame; long len; int lx, ly, rx, ry; } sticks[64];
    int nsticks;
    struct { long frame; long len; int dx, dy; } mice[32];
    int nmice;
    float scale;       /* window scale */
    int render_scale;  /* internal resolution multiplier */
    bool software;
    bool renderer_set;
    bool smooth;
    float mouse_sens;
    long fuzz_from;     /* random input from this frame on (-1 = off) */
    uint32_t fuzz_seed;
    bool fuzz_start;
    float aspect;
    bool no_vsync;
    bool show_fps;
    bool no_pgxp;
    const char *save_dir;
    bool no_launcher;
    const char *launcher_shot, *launcher_page, *launcher_keys;
    const char *overlay_shot;
} opt = {.max_frames = -1, .scale = 3.0f, .render_scale = 4, .smooth = true,
       .mouse_sens = 6.0f, .fuzz_from = -1, .aspect = 16.0f / 9.0f};

static SDL_Window *g_win;
static bool g_gpu_ok;
static SDL_AudioStream *g_audio;
static SDL_Gamepad *g_pad;
static uint64_t g_next_frame_ns;
static bool g_vsync_paced;  /* display refresh ~= PS1 refresh: let vsync pace */
static uint64_t g_frame_ns = (uint64_t)FRAME_NS; /* host time per emulated frame */
static bool g_quit;
static bool g_turbo;
static long g_frame;
static uint8_t *g_rgb;      /* display conversion buffer (RGB24) */
static Settings g_cfg;      /* user settings for this run (config.ini + CLI) */
static Settings g_saved;    /* contents of config.ini (no command-line overrides) */
static Settings g_applied;  /* the settings currently in effect */

/* in-game overlay (F2, or Select+Start held on a controller) */
#define OVERLAY_COMBO_FRAMES 20
static OverlayInfo g_overlay_info;
static bool g_overlay_want;    /* open it once this vblank's input is read */
static int g_combo_frames;
static bool g_combo_latched;   /* Select+Start kept from the game until released */
static bool g_input_hold;      /* after closing: buttons still held stay with the overlay */

/* --overlay-keys: scripted input for testing the overlay. Tokens are SDL
 * key names, "pad:BUTTON[+BUTTON][:TICKS]" (a virtual controller),
 * "WAITn" and "SHOT" (window image with overlay, see --overlay-shot). One
 * token is processed every few ticks: a tick is a vblank while the game
 * runs and a frame while the overlay is open. */
enum { TOK_KEY, TOK_PAD, TOK_WAIT, TOK_SHOT };
static struct {
    long start;
    struct { int kind, code, arg; } tok[128];
    int n, pos, tick, shots;
    bool uses_pad;
    SDL_Joystick *vpad;
} g_script;

/* ------------------------------------------------------------------ */
/* display conversion                                                  */

static void display_to_rgb(const DisplayInfo *d, uint8_t *out, int *ow, int *oh) {
    int w = d->w, h = d->h;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    for (int y = 0; y < h; y++) {
        int vy = (d->y + y) & 511;
        const uint16_t *row = &g_vram[vy * VRAM_W];
        uint8_t *dst = out + (size_t)y * (size_t)w * 3;
        if (d->rgb24) {
            const uint8_t *bytes = (const uint8_t *)row;
            for (int x = 0; x < w; x++) {
                int bo = d->x * 2 + x * 3;
                dst[x * 3 + 0] = bytes[(bo + 0) & 2047];
                dst[x * 3 + 1] = bytes[(bo + 1) & 2047];
                dst[x * 3 + 2] = bytes[(bo + 2) & 2047];
            }
        } else {
            for (int x = 0; x < w; x++) {
                uint16_t p = row[(d->x + x) & 1023];
                int r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
                dst[x * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
                dst[x * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
                dst[x * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
            }
        }
    }
    *ow = w;
    *oh = h;
}

static void write_shot(const char *path, bool vram) {
    if (vram) {
        if (g_gpu_backend->vram_sync)
            g_gpu_backend->vram_sync(0, 0, VRAM_W, VRAM_H);
        uint8_t *rgb = malloc(VRAM_W * VRAM_H * 3);
        for (int i = 0; i < VRAM_W * VRAM_H; i++) {
            uint16_t p = g_vram[i];
            rgb[i * 3 + 0] = (uint8_t)((p & 31) << 3);
            rgb[i * 3 + 1] = (uint8_t)(((p >> 5) & 31) << 3);
            rgb[i * 3 + 2] = (uint8_t)(((p >> 10) & 31) << 3);
        }
        png_write_rgb(path, rgb, VRAM_W, VRAM_H);
        free(rgb);
    } else if (g_gpu_ok && !opt.software) {
        uint8_t *rgb;
        int w, h;
        if (render_capture(&rgb, &w, &h)) {
            png_write_rgb(path, rgb, w, h);
            free(rgb);
        }
    } else {
        DisplayInfo d;
        gpu_display_info(&d);
        int w, h;
        display_to_rgb(&d, g_rgb, &w, &h);
        png_write_rgb(path, g_rgb, w, h);
    }
    LOGI("main", "frame %ld: wrote %s", g_frame, path);
}

/* ------------------------------------------------------------------ */
/* input                                                               */

static uint16_t scripted_buttons(bool *sprint) {
    uint16_t b = 0;
    for (int i = 0; i < opt.npresses; i++)
        if (g_frame >= opt.presses[i].frame &&
            g_frame < opt.presses[i].frame + opt.presses[i].len) {
            b |= opt.presses[i].button;
            *sprint |= opt.presses[i].sprint;
        }
    return b;
}

/* mouse look: relative motion accumulated between vblanks turns the camera
 * directly (camera.h), MOUSE_LOOK_UNITS game angle units (4096 = a full
 * turn) per count at sensitivity 1 */
#define MOUSE_LOOK_UNITS 0.2f
static float g_mouse_dx, g_mouse_dy;
static bool g_mouse_captured;
static uint32_t g_mouse_buttons;

static void mouse_look(float dx, float dy) {
    /* dropped when the game has not turned for a few vblanks (menus,
     * pause), so it is not applied later in one go */
    if (++g_cam_mouse_age > 4)
        g_cam_mouse_x = g_cam_mouse_y = 0;
    float k = opt.mouse_sens * MOUSE_LOOK_UNITS;
    g_cam_mouse_x += dx * k;
    g_cam_mouse_y += dy * k;
    if (fabsf(g_cam_mouse_x) > 4096) g_cam_mouse_x = copysignf(4096, g_cam_mouse_x);
    if (fabsf(g_cam_mouse_y) > 4096) g_cam_mouse_y = copysignf(4096, g_cam_mouse_y);
}

/* Select+Start held together on the controller (the buttons bound to the
 * PlayStation's Select and Start) opens the overlay. While both are down
 * neither reaches the game, and each stays masked until released, so the
 * game's own pause menu does not open as well. */
static bool overlay_combo(void) {
    if (!g_pad) {
        g_combo_frames = 0;
        g_combo_latched = false;
        return false;
    }
    int sel = g_cfg.pad[ACT_SELECT] >= 0 ? g_cfg.pad[ACT_SELECT] : SDL_GAMEPAD_BUTTON_BACK;
    int st = g_cfg.pad[ACT_START] >= 0 ? g_cfg.pad[ACT_START] : SDL_GAMEPAD_BUTTON_START;
    bool a = input_pad_held(g_pad, sel), b = input_pad_held(g_pad, st);
    if (a && b) {
        g_combo_latched = true;
        if (++g_combo_frames == OVERLAY_COMBO_FRAMES)
            g_overlay_want = true;
    } else {
        g_combo_frames = 0;
        if (!a && !b)
            g_combo_latched = false;
    }
    return g_combo_latched;
}

static void poll_input(void) {
    bool sprint = false;
    uint16_t b = scripted_buttons(&sprint);
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    float mdx = 0, mdy = 0;
    for (int i = 0; i < opt.nmice; i++)
        if (g_frame >= opt.mice[i].frame && g_frame < opt.mice[i].frame + opt.mice[i].len)
            mdx += (float)opt.mice[i].dx, mdy += (float)opt.mice[i].dy;
    for (int i = 0; i < opt.nsticks; i++) {
        if (g_frame >= opt.sticks[i].frame &&
            g_frame < opt.sticks[i].frame + opt.sticks[i].len) {
            lx = (uint8_t)opt.sticks[i].lx;
            ly = (uint8_t)opt.sticks[i].ly;
            rx = (uint8_t)opt.sticks[i].rx;
            ry = (uint8_t)opt.sticks[i].ry;
        }
    }
    if (opt.fuzz_from >= 0 && g_frame >= opt.fuzz_from) {
        /* random play: new input every 20 frames, START rarely */
        static uint32_t st;
        static uint16_t fb;
        static uint8_t f[4];
        if (!st)
            st = opt.fuzz_seed | 1;
        if (g_frame % 20 == 0) {
            st = st * 1664525u + 1013904223u;
            fb = (uint16_t)(st >> 16) & (uint16_t)~PAD_START & (uint16_t)~PAD_SELECT;
            st = st * 1664525u + 1013904223u;
            if (opt.fuzz_start && (st >> 24) == 7)
                fb |= PAD_START;
            for (int i = 0; i < 4; i++) {
                st = st * 1664525u + 1013904223u;
                f[i] = (uint8_t)(st >> 24);
            }
            f[1] = (uint8_t)(f[1] / 2); /* bias towards walking forward */
        }
        b |= fb;
        lx = f[0], ly = f[1], rx = f[2], ry = (uint8_t)(0x60 + f[3] / 4);
    }
    if (!opt.headless) {
        /* keyboard, mouse buttons and controller through the bindings
         * from the launcher's Controls page (defaults in settings.c) */
        InputState in;
        input_read(&g_cfg, g_pad, g_mouse_captured ? g_mouse_buttons : 0, &in);
        if (overlay_combo())
            in.buttons &= (uint16_t)~(PAD_SELECT | PAD_START);
        if (g_input_hold) {
            /* the key or button that closed the overlay is still down */
            if (in.buttons)
                in.buttons = 0;
            else
                g_input_hold = false;
        }
        b |= in.buttons;
        if (in.lx != 0x80) lx = in.lx;
        if (in.ly != 0x80) ly = in.ly;
        if (in.rx != 0x80) rx = in.rx;
        if (in.ry != 0x80) ry = in.ry;
        sprint |= in.sprint;
        g_turbo = in.fast_forward;
        if (g_mouse_captured && g_cfg.mouse_look)
            mdx += g_mouse_dx, mdy += g_mouse_dy;
        g_mouse_dx = g_mouse_dy = 0;
    }
    mouse_look(mdx, mdy);
    pad_set_buttons(0, b);
    pad_set_analog(0, rx, ry, lx, ly);
    g_sprint = sprint;
}

/* ------------------------------------------------------------------ */
/* platform hooks                                                      */

static FILE *g_wav;
static uint32_t g_wav_frames;

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                     'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0,
                     'd', 'a', 't', 'a', 0, 0, 0, 0};
    uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void wav_close(void) {
    if (g_wav) {
        wav_header(g_wav, g_wav_frames);
        fclose(g_wav);
        g_wav = NULL;
    }
}

void platform_audio_push(const int16_t *stereo, int frames) {
    if (g_wav) {
        fwrite(stereo, 4, (size_t)frames, g_wav);
        g_wav_frames += (uint32_t)frames;
    }
    if (!g_audio || g_turbo)
        return;
    /* keep latency bounded: drop if more than ~150 ms queued */
    if (SDL_GetAudioStreamQueued(g_audio) > 44100 * 4 * 15 / 100)
        return;
    SDL_PutAudioStreamData(g_audio, stereo, frames * 4);
}

/* Keep the audio queue near a target latency by nudging the playback rate.
 * This absorbs the small difference between the PS1 refresh (59.82 Hz) and
 * the display refresh when vsync paces emulation, without drops or clicks. */
static void audio_rate_control(void) {
    if (!g_audio)
        return;
    const float target = 44100.0f * 4.0f * 0.06f; /* ~60 ms */
    float queued = (float)SDL_GetAudioStreamQueued(g_audio);
    float ratio = 1.0f + (queued - target) / target * 0.01f;
    if (ratio < 0.98f) ratio = 0.98f;
    if (ratio > 1.02f) ratio = 1.02f;
    SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
}

const char *platform_data_dir(void) {
    static char dir[1024];
    if (!dir[0] && opt.save_dir) {
        snprintf(dir, sizeof dir, "%s", opt.save_dir);
    } else if (!dir[0]) {
        const char *pref = SDL_GetPrefPath("AlienResurrectionPort", "AlienResurrection");
        if (pref) {
            snprintf(dir, sizeof dir, "%s", pref);
            size_t n = strlen(dir);
            if (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
                dir[n - 1] = 0;
        } else {
            snprintf(dir, sizeof dir, "saves");
        }
    }
    return dir;
}

bool platform_should_quit(void) { return g_quit; }

/* game frame rate = display buffer flips per second of emulated time */
static void fps_tick(void) {
    static uint64_t last_flips;
    static long last_frame;
    if (g_frame - last_frame < 60)
        return;
    double fps = (double)(g_gpu_flip_counter - last_flips) * 59.82 /
                 (double)(g_frame - last_frame);
    last_flips = g_gpu_flip_counter;
    last_frame = g_frame;
    if (opt.headless) {
        LOGI("fps", "frame %ld: game %.1f fps", g_frame, fps);
    } else {
        char t[96];
        snprintf(t, sizeof t, "Alien Resurrection - %.0f fps", fps);
        SDL_SetWindowTitle(g_win, t);
    }
}

/* ------------------------------------------------------------------ */
/* settings that apply while the game runs                             */

static uint32_t cycle_scale_for(int cpu_pct) {
    uint32_t s = (uint32_t)(32 * 100 / (cpu_pct < 50 ? 50 : cpu_pct));
    return s < 1 ? 1 : s;
}

static float settings_aspect(const Settings *c) {
    return c->widescreen ? (float)c->aspect_w / (float)c->aspect_h : 4.0f / 3.0f;
}

static void apply_camera(const Settings *c) {
    g_stick_direct = c->stick_direct;
    g_cam_stick_speed = c->stick_speed;
    g_stick_dead_zone = c->stick_dead_zone;
    g_cam_stick_curve = g_stick_curves[c->stick_curve].exponent;
}

/* Apply what changed in g_cfg since the last call (overlay edits).
 * Bindings and mouse look are read from g_cfg directly. Internal
 * resolution, vsync and skip intro only take effect at the next start. */
static void apply_live(void) {
    Settings *c = &g_cfg, *a = &g_applied;
    if (c->pgxp != a->pgxp) {
        bool on = c->pgxp && !opt.software;
        if (on && !g_pgxp)
            pgxp_reset();
        g_pgxp = on;
        opt.no_pgxp = !c->pgxp;
        LOGI("overlay", "precision geometry %s", on ? "on" : "off");
    }
    if (c->widescreen != a->widescreen || c->aspect_w != a->aspect_w ||
        c->aspect_h != a->aspect_h) {
        opt.aspect = settings_aspect(c);
        if (!opt.software)
            ws_configure(opt.aspect);
        if (c->widescreen)
            LOGI("overlay", "widescreen %d:%d", c->aspect_w, c->aspect_h);
        else
            LOGI("overlay", "widescreen off (4:3)");
    }
    if (c->smooth != a->smooth) {
        opt.smooth = c->smooth;
        render_set_smooth(c->smooth);
        LOGI("overlay", "smooth scaling %s", c->smooth ? "on" : "off");
    }
    if (c->fullscreen != a->fullscreen) {
        SDL_SetWindowFullscreen(g_win, c->fullscreen);
        LOGI("overlay", "fullscreen %s", c->fullscreen ? "on" : "off");
    }
    if (c->show_fps != a->show_fps) {
        opt.show_fps = c->show_fps;
        if (!c->show_fps)
            SDL_SetWindowTitle(g_win, "Alien Resurrection");
        LOGI("overlay", "frame rate display %s", c->show_fps ? "on" : "off");
    }
    if (c->sprint != a->sprint || c->sprint_pct != a->sprint_pct) {
        g_sprint_pct = c->sprint ? c->sprint_pct : 100;
        LOGI("overlay", "sprint speed %d%%", g_sprint_pct);
    }
    if (c->infinite_flashlight != a->infinite_flashlight) {
        g_infinite_flashlight = c->infinite_flashlight;
        LOGI("overlay", "infinite flashlight %s", c->infinite_flashlight ? "on" : "off");
    }
    if (c->enemy_damage_pct != a->enemy_damage_pct ||
        memcmp(c->enemy_damage_group_pct, a->enemy_damage_group_pct,
               sizeof c->enemy_damage_group_pct)) {
        g_enemy_damage_pct = c->enemy_damage_pct;
        memcpy(g_enemy_damage_group_pct, c->enemy_damage_group_pct,
               sizeof g_enemy_damage_group_pct);
        char buf[256];
        int n = snprintf(buf, sizeof buf, "all %d%%", c->enemy_damage_pct);
        for (int g = 0; g < DMG_GROUP_COUNT && n < (int)sizeof buf; g++)
            n += snprintf(buf + n, sizeof buf - (size_t)n, ", %s %d%%", g_enemy_groups[g].id,
                          c->enemy_damage_group_pct[g]);
        LOGI("overlay", "enemy damage: %s", buf);
    }
    if (c->cpu_pct != a->cpu_pct) {
        g_cycle_scale = cycle_scale_for(c->cpu_pct);
        LOGI("overlay", "emulated CPU speed %d%%", c->cpu_pct);
    }
    if (c->mouse_look != a->mouse_look || c->mouse_sens != a->mouse_sens) {
        opt.mouse_sens = c->mouse_sens;
        LOGI("overlay", "mouse look %s, sensitivity %.1f", c->mouse_look ? "on" : "off",
             (double)c->mouse_sens);
    }
    if (c->stick_direct != a->stick_direct || c->stick_speed != a->stick_speed ||
        c->stick_dead_zone != a->stick_dead_zone || c->stick_curve != a->stick_curve) {
        apply_camera(c);
        LOGI("overlay", "sticks: %s, dead zone %d%%; camera stick speed %d%%, curve %s",
             c->stick_direct ? "responsive" : "original", c->stick_dead_zone, c->stick_speed,
             g_stick_curves[c->stick_curve].id);
    }
    if (memcmp(c->key, a->key, sizeof c->key) || memcmp(c->pad, a->pad, sizeof c->pad))
        LOGI("overlay", "bindings changed");
    *a = *c;
}

/* ------------------------------------------------------------------ */
/* scripted overlay testing (--overlay-keys / --overlay-shot)          */

static void script_parse(const char *v) {
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", v);
    char *colon = strchr(buf, ':');
    if (!colon) {
        fprintf(stderr, "bad --overlay-keys %s (use FRAME:KEY,KEY,...)\n", v);
        exit(1);
    }
    *colon = 0;
    g_script.start = atol(buf);
    for (char *tok = strtok(colon + 1, ","); tok; tok = strtok(NULL, ",")) {
        if (g_script.n >= (int)(sizeof g_script.tok / sizeof g_script.tok[0]))
            break;
        typeof(g_script.tok[0]) *t = &g_script.tok[g_script.n++];
        if (!strncmp(tok, "WAIT", 4)) {
            t->kind = TOK_WAIT;
            t->arg = tok[4] ? atoi(tok + 4) : 1;
        } else if (!strcmp(tok, "SHOT")) {
            t->kind = TOK_SHOT;
        } else if (!strncmp(tok, "pad:", 4)) {
            t->kind = TOK_PAD;
            t->arg = 3;
            char *len = strchr(tok + 4, ':');
            if (len) {
                *len = 0;
                t->arg = atoi(len + 1) < 1 ? 1 : atoi(len + 1);
            }
            for (char *p = tok + 4; p;) {
                char *plus = strchr(p, '+');
                if (plus)
                    *plus = 0;
                SDL_GamepadButton gb = SDL_GetGamepadButtonFromString(p);
                if (gb == SDL_GAMEPAD_BUTTON_INVALID) {
                    fprintf(stderr, "unknown controller button '%s' in --overlay-keys\n", p);
                    exit(1);
                }
                t->code |= 1 << gb;
                p = plus ? plus + 1 : NULL;
            }
            g_script.uses_pad = true;
        } else {
            t->kind = TOK_KEY;
            t->code = (int)SDL_GetScancodeFromName(tok);
            if (t->code == SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "unknown key '%s' in --overlay-keys\n", tok);
                exit(1);
            }
        }
    }
}

/* A virtual controller driven by the "pad:" tokens. */
static SDL_JoystickID script_attach_pad(void) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "Scripted test controller";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    g_script.vpad = id ? SDL_OpenJoystick(id) : NULL;
    if (!g_script.vpad) {
        LOGW("main", "virtual controller: %s", SDL_GetError());
        return 0;
    }
    /* triggers at rest */
    SDL_SetJoystickVirtualAxis(g_script.vpad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(g_script.vpad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    return id;
}

static void write_window_shot(void) {
    uint8_t *rgb;
    int w, h;
    char path[600];
    snprintf(path, sizeof path, "%s_%d.png", opt.overlay_shot ? opt.overlay_shot : "overlay",
             ++g_script.shots);
    if (render_capture_window(&rgb, &w, &h)) {
        png_write_rgb(path, rgb, w, h);
        free(rgb);
        LOGI("main", "frame %ld: wrote %s", g_frame, path);
    }
}

static bool script_done(void) { return g_script.pos >= g_script.n; }

static void script_tick(void) {
    if (script_done() || g_frame < g_script.start)
        return;
    typeof(g_script.tok[0]) *t = &g_script.tok[g_script.pos];
    int k = g_script.tick++;
    bool next = false;
    switch (t->kind) {
    case TOK_KEY:
        if (k <= 1) {
            SDL_Event e = {0};
            e.type = k == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = SDL_GetWindowID(g_win);
            e.key.scancode = (SDL_Scancode)t->code;
            e.key.key = SDL_GetKeyFromScancode(e.key.scancode, SDL_KMOD_NONE, false);
            e.key.down = k == 0;
            SDL_PushEvent(&e);
        }
        next = k >= 3;
        break;
    case TOK_PAD:
        if (g_script.vpad && (k == 0 || k == t->arg))
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
                if (t->code & (1 << b))
                    SDL_SetJoystickVirtualButton(g_script.vpad, b, k == 0);
        next = k >= t->arg + 2;
        break;
    case TOK_WAIT:
        next = k + 1 >= t->arg;
        break;
    case TOK_SHOT:
        /* a few frames after the previous input, so the UI has settled */
        if (k == 6)
            write_window_shot();
        next = k >= 6;
        break;
    }
    if (next) {
        g_script.pos++;
        g_script.tick = 0;
    }
}

/* ------------------------------------------------------------------ */
/* in-game overlay                                                     */

/* Runs the overlay until the player resumes. The emulated machine is
 * paused meanwhile (this runs inside the vblank event), as is the audio;
 * the last game frame stays on screen under the menu. */
static void run_overlay(void) {
    if (!g_gpu_ok || !g_win)
        return;
    if (!overlay_open(g_win, render_gpu_device(), render_window_format(), &g_cfg, &g_saved,
                      settings_path(), &g_overlay_info)) {
        LOGW("overlay", "cannot open the overlay (no GPU device)");
        return;
    }
    LOGI("overlay", "opened at frame %ld", g_frame);
    bool was_captured = g_mouse_captured;
    g_mouse_captured = false;
    g_mouse_buttons = 0;
    g_cam_mouse_x = g_cam_mouse_y = 0;
    SDL_SetWindowRelativeMouseMode(g_win, false);
    if (g_audio)
        SDL_PauseAudioStreamDevice(g_audio);
    float aspect_before = opt.aspect;
    render_hold_display_aspect(true);
    render_set_overlay(overlay_render);

    int result = OVERLAY_OPEN;
    int idle = 0;
    uint64_t next = SDL_GetTicksNS();
    while (result == OVERLAY_OPEN) {
        script_tick();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                result = OVERLAY_QUIT;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!g_pad)
                    g_pad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) {
                    SDL_CloseGamepad(g_pad);
                    g_pad = NULL;
                }
                break;
            default:
                break;
            }
            overlay_event(&e);
        }
        if (result != OVERLAY_OPEN)
            break;
        if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }
        result = overlay_update();
        apply_live();
        /* scripted test runs (--frames) must not hang in a forgotten overlay */
        if (opt.max_frames >= 0 && g_script.n && script_done() && ++idle > 600) {
            LOGW("overlay", "test run: closing the idle overlay");
            result = OVERLAY_RESUME;
        }
        if (result != OVERLAY_OPEN)
            break;
        render_present();
        uint64_t now = SDL_GetTicksNS();
        next += g_frame_ns;
        if (next > now)
            SDL_DelayNS(next - now);
        else if (now - next > 100000000ull)
            next = now;
    }
    overlay_close();
    render_set_overlay(NULL);
    render_hold_display_aspect(false);
    if (result == OVERLAY_QUIT) {
        LOGI("overlay", "quit to desktop");
        exit(0);
    }
    LOGI("overlay", "resumed");
    if (g_audio)
        SDL_ResumeAudioStreamDevice(g_audio);
    if (was_captured) {
        g_mouse_captured = true;
        SDL_SetWindowRelativeMouseMode(g_win, true);
    }
    /* a new aspect ratio: reshape the window like at start-up */
    if (opt.aspect != aspect_before && !g_cfg.fullscreen &&
        !(SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int w, h;
        SDL_GetWindowSize(g_win, &w, &h);
        SDL_SetWindowSize(g_win, (int)((float)h * opt.aspect + 0.5f), h);
    }
    g_input_hold = true;
    g_next_frame_ns = 0; /* no catching up on the paused time */
}

void platform_vblank(void) {
    g_frame++;
    if (opt.show_fps)
        fps_tick();
    for (int i = 0; i < opt.nshots; i++)
        if (opt.shots[i].frame == g_frame)
            write_shot(opt.shots[i].path, opt.shots[i].vram);
    if (opt.record.count && g_frame >= opt.record.start &&
        (g_frame - opt.record.start) % opt.record.step == 0 &&
        (g_frame - opt.record.start) / opt.record.step < opt.record.count) {
        char path[600];
        snprintf(path, sizeof path, "%s_%04ld.png", opt.record.prefix,
                 (g_frame - opt.record.start) / opt.record.step);
        write_shot(path, false);
    }
    for (int i = 0; i < opt.ndumps; i++)
        if (opt.dumps[i].frame == g_frame) {
            FILE *f = fopen(opt.dumps[i].path, "wb");
            if (f) {
                fwrite(g_ram, 1, sizeof g_ram, f);
                fclose(f);
                LOGI("main", "frame %ld: dumped RAM to %s", g_frame, opt.dumps[i].path);
            }
        }
    for (int i = 0; i < opt.npokes; i++)
        if (opt.pokes[i].frame == g_frame) {
            uint32_t a = opt.pokes[i].addr & 0x1FFFFFu & ~(uint32_t)(opt.pokes[i].size - 1);
            memcpy(&g_ram[a], &opt.pokes[i].value, (size_t)opt.pokes[i].size); /* little endian */
            LOGI("main", "frame %ld: poked %08X = %X", g_frame, opt.pokes[i].addr,
                 opt.pokes[i].value);
        }
    if (opt.max_frames >= 0 && g_frame >= opt.max_frames) {
        LOGI("main", "reached frame limit %ld", g_frame);
        exit(0);
    }
    if (opt.headless) {
        poll_input();
        /* no window, but the frame's GPU work must still be submitted:
         * otherwise one command buffer collects every frame's draws and
         * uploads until the next screenshot, and memory grows without
         * bound (tens of GB on long runs) */
        if (g_gpu_ok)
            render_present();
        return;
    }
    script_tick();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            g_quit = true;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_pad)
                g_pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) {
                SDL_CloseGamepad(g_pad);
                g_pad = NULL;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (g_mouse_captured) {
                g_mouse_dx += e.motion.xrel;
                g_mouse_dy += e.motion.yrel;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!g_mouse_captured) {
                g_mouse_captured = true;
                SDL_SetWindowRelativeMouseMode(g_win, true);
            } else {
                g_mouse_buttons |= SDL_BUTTON_MASK(e.button.button);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            g_mouse_buttons &= ~SDL_BUTTON_MASK(e.button.button);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            g_mouse_captured = false;
            g_mouse_buttons = 0;
            SDL_SetWindowRelativeMouseMode(g_win, false);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_F1) {
                g_mouse_captured = false;
                g_mouse_buttons = 0;
                SDL_SetWindowRelativeMouseMode(g_win, false);
            } else if (e.key.scancode == SDL_SCANCODE_F2 && !e.key.repeat) {
                g_overlay_want = true;
            } else if (e.key.scancode == SDL_SCANCODE_F12) {
                /* screenshot; Shift+F12 dumps the whole VRAM */
                bool vram = (e.key.mod & SDL_KMOD_SHIFT) != 0;
                char p[64];
                snprintf(p, sizeof p, "%s_%05ld.png", vram ? "vram" : "shot", g_frame);
                write_shot(p, vram);
            } else if (!e.key.repeat &&
                       (e.key.scancode == SDL_SCANCODE_F11 ||
                        (e.key.scancode == SDL_SCANCODE_F && (e.key.mod & SDL_KMOD_GUI) &&
                         (e.key.mod & SDL_KMOD_CTRL)))) {
                g_cfg.fullscreen = !g_cfg.fullscreen;
                g_applied.fullscreen = g_cfg.fullscreen;
                SDL_SetWindowFullscreen(g_win, g_cfg.fullscreen);
            }
            break;
        default:
            break;
        }
    }
    if (g_quit) {
        LOGI("main", "quit requested");
        exit(0);
    }
    poll_input();
    if (g_overlay_want) {
        g_overlay_want = false;
        run_overlay();
        poll_input();
    }
    render_present();
    audio_rate_control();
    /* pacing: a timer caps the frame rate (at the display refresh when it is
     * close to the PS1's, so vsync'd presentation never judders; otherwise
     * at the PS1 rate). Under vsync the swapchain wait aligns frames with
     * the refresh; the timer keeps things correct when that wait does not
     * block (occluded window, high-refresh displays). */
    uint64_t now = SDL_GetTicksNS();
    if (!g_next_frame_ns)
        g_next_frame_ns = now;
    g_next_frame_ns += g_frame_ns;
    if (g_turbo) {
        g_next_frame_ns = now;
    } else if (g_next_frame_ns > now) {
        SDL_DelayNS(g_next_frame_ns - now);
    } else if (now - g_next_frame_ns > 100000000ull) {
        g_next_frame_ns = now; /* fell behind: resync */
    }
}

static void vblank_event(int64_t now, int64_t late) {
    (void)late;
    irq_raise(IRQ_VBLANK);
    gpu_vblank();
    platform_vblank();
    sched_at(EV_VBLANK, now + FRAME_CYCLES);
}

/* ------------------------------------------------------------------ */

static uint16_t button_by_name(const char *n) {
    static const struct { const char *n; uint16_t b; } t[] = {
        {"select", PAD_SELECT}, {"start", PAD_START}, {"up", PAD_UP},
        {"right", PAD_RIGHT}, {"down", PAD_DOWN}, {"left", PAD_LEFT},
        {"l2", PAD_L2}, {"r2", PAD_R2}, {"l1", PAD_L1}, {"r1", PAD_R1},
        {"triangle", PAD_TRIANGLE}, {"circle", PAD_CIRCLE},
        {"cross", PAD_CROSS}, {"square", PAD_SQUARE},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].n, n))
            return t[i].b;
    fprintf(stderr, "unknown button %s\n", n);
    exit(1);
}

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--headless")) {
            opt.headless = true;
        } else if (!strcmp(a, "--frames") && v) {
            opt.max_frames = atol(v), i++;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: alien [options]\n"
                   "Without options the launcher opens (disc image, settings, controls).\n"
                   "Settings given here override config.ini for this run.\n"
                   "  --play          start the game directly (no launcher)\n"
                   "  --disc PATH     disc image: the .cue file or the folder containing it\n"
                   "  --skip-intro    skip the boot loader and intro movies\n"
                   "  --fullscreen    start in fullscreen\n"
                   "  --aspect W:H    display aspect, e.g. 16:9 (default), 16:10, 21:9, 4:3\n"
                   "  --scale N       internal resolution multiplier (default 4)\n"
                   "  --renderer sw   software renderer\n"
                   "  --sharp         nearest-neighbour scaling to the window\n"
                   "  --no-vsync      present immediately (may tear, lowest latency)\n"
                   "  --show-fps      show the game's frame rate in the window title\n"
                   "  --no-pgxp       original integer geometry and affine textures\n"
#ifndef __APPLE__
                   "  --gpu-driver API  auto (default), vulkan or direct3d12\n"
#endif
                   "  --cpu-speed PCT emulated CPU speed in percent (default 300; 100 = original)\n"
                   "  --sprint-speed PCT  movement speed while Shift is held (default 190; 100 = off)\n"
                   "  --no-infinite-flashlight  the flashlight battery runs down as in the original\n"
                   "  --enemy-damage PCT|GROUP=PCT  damage taken from enemies (default 100;\n"
                   "                  groups as in config.ini [enemy_damage]; repeatable)\n"
                   "  --mouse-sens N  mouse-look sensitivity (default 6)\n"
                   "  --original-stick  both sticks through the game's own input path\n"
                   "                  (two frames of delay, 38%% dead zone)\n"
                   "  --save-dir DIR  folder for the memory card images\n"
                   "In game, F2 (or Select + Start held on a controller) opens the settings overlay.\n"
                   "See README.md for testing and debugging options.\n");
            exit(0);
        } else if ((!strcmp(a, "--disc") || !strcmp(a, "--cue")) && v) {
            snprintf(g_cfg.disc, sizeof g_cfg.disc, "%s", v), i++;
        } else if (!strcmp(a, "--exe") && v) {
            opt.exe = v, i++;
        } else if (!strcmp(a, "--log") && v) {
            g_log_level = atoi(v), i++;
        } else if (!strcmp(a, "--watch") && v) {
            /* ADDR[:LEN] (hex) */
            char *end;
            g_watch_lo = (uint32_t)strtoul(v, &end, 16) & 0x1FFFFF;
            g_watch_len = (*end == ':') ? (uint32_t)strtoul(end + 1, NULL, 16) : 4;
            i++;
        } else if (!strcmp(a, "--verify") && v) {
            extern uint32_t g_verify_addr[16];
            extern int g_verify_count;
            if (g_verify_count < 16)
                g_verify_addr[g_verify_count++] =
                    0x80000000u | ((uint32_t)strtoul(v, NULL, 16) & 0x1FFFFFFF);
            i++;
        } else if (!strcmp(a, "--verify-limit") && v) {
            extern int g_verify_limit;
            g_verify_limit = atoi(v), i++;
        } else if (!strcmp(a, "--trace-bios")) {
            extern int g_bios_trace;
            g_bios_trace = 1;
        } else if (!strcmp(a, "--window-scale") && v) {
            opt.scale = (float)atof(v), i++;
        } else if (!strcmp(a, "--scale") && v) {
            g_cfg.internal_scale = atoi(v) < 1 ? 1 : atoi(v) > 8 ? 8 : atoi(v), i++;
        } else if (!strcmp(a, "--aspect") && v) {
            /* W:H, e.g. 16:9, 16:10, 21:9; 4:3 = original */
            int aw = 0, ah = 0;
            if (sscanf(v, "%d:%d", &aw, &ah) == 2 && aw > 0 && ah > 0) {
                g_cfg.widescreen = aw * 3 != ah * 4;
                if (g_cfg.widescreen)
                    g_cfg.aspect_w = aw, g_cfg.aspect_h = ah;
            } else {
                fprintf(stderr, "bad --aspect %s (use e.g. 16:9)\n", v);
                exit(1);
            }
            i++;
        } else if (!strcmp(a, "--renderer") && v) {
            opt.software = !strcmp(v, "sw") || !strcmp(v, "software");
            opt.renderer_set = true;
            i++;
        } else if (!strcmp(a, "--fuzz") && v) {
            /* FROM:SEED */
            int st = 0;
            sscanf(v, "%ld:%u:%d", &opt.fuzz_from, &opt.fuzz_seed, &st);
            opt.fuzz_start = st != 0;
            i++;
        } else if (!strcmp(a, "--wav") && v) {
            g_wav = fopen(v, "wb");
            if (g_wav)
                wav_header(g_wav, 0);
            atexit(wav_close);
            i++;
        } else if (!strcmp(a, "--mouse-sens") && v) {
            g_cfg.mouse_sens = (float)atof(v), i++;
        } else if (!strcmp(a, "--original-stick")) {
            g_cfg.stick_direct = false;
        } else if (!strcmp(a, "--mouse") && v) {
            /* FRAME:LEN:DX:DY scripted mouse motion per vblank (counts) */
            long f, len;
            int dx, dy;
            if (sscanf(v, "%ld:%ld:%d:%d", &f, &len, &dx, &dy) == 4 && opt.nmice < 32)
                opt.mice[opt.nmice++] = (typeof(opt.mice[0])){f, len, dx, dy};
            i++;
        } else if (!strcmp(a, "--cpu-speed") && v) {
            /* percent of the original CPU throughput */
            int pct = atoi(v);
            g_cfg.cpu_pct = pct < 50 ? 50 : pct > 800 ? 800 : pct;
            i++;
        } else if (!strcmp(a, "--sprint-speed") && v) {
            int pct = atoi(v);
            g_cfg.sprint = pct > 100;
            if (g_cfg.sprint)
                g_cfg.sprint_pct = pct > 300 ? 300 : pct;
            i++;
        } else if (!strcmp(a, "--save-dir") && v) {
            opt.save_dir = v, i++;
        } else if (!strcmp(a, "--gpu-driver") && v) {
            /* Windows: auto, vulkan or direct3d12 */
            snprintf(g_cfg.gpu_driver, sizeof g_cfg.gpu_driver, "%s", v), i++;
        } else if (!strcmp(a, "--no-pgxp")) {
            g_cfg.pgxp = false;
        } else if (!strcmp(a, "--no-infinite-flashlight")) {
            g_cfg.infinite_flashlight = false;
        } else if (!strcmp(a, "--enemy-damage") && v) {
            /* PCT (all enemies) or GROUP=PCT */
            const char *eq = strchr(v, '=');
            int pct = atoi(eq ? eq + 1 : v), g = DMG_GROUP_COUNT;
            pct = pct < 0 ? 0 : pct > ENEMY_DAMAGE_MAX ? ENEMY_DAMAGE_MAX : pct;
            if (eq)
                for (g = 0; g < DMG_GROUP_COUNT; g++)
                    if (!strncmp(v, g_enemy_groups[g].id, (size_t)(eq - v)) &&
                        !g_enemy_groups[g].id[eq - v])
                        break;
            if (!eq)
                g_cfg.enemy_damage_pct = pct;
            else if (g < DMG_GROUP_COUNT)
                g_cfg.enemy_damage_group_pct[g] = pct;
            else {
                fprintf(stderr, "unknown enemy group in --enemy-damage %s\n", v);
                exit(1);
            }
            i++;
        } else if (!strcmp(a, "--show-fps")) {
            g_cfg.show_fps = true;
        } else if (!strcmp(a, "--no-vsync")) {
            g_cfg.vsync = false;
        } else if (!strcmp(a, "--sharp")) {
            g_cfg.smooth = false;
        } else if (!strcmp(a, "--fullscreen")) {
            g_cfg.fullscreen = true;
        } else if (!strcmp(a, "--skip-intro")) {
            g_cfg.skip_intro = true;
        } else if (!strcmp(a, "--no-launcher") || !strcmp(a, "--play")) {
            opt.no_launcher = true;
        } else if (!strcmp(a, "--launcher-shot") && v) {
            opt.launcher_shot = v, i++;
        } else if (!strcmp(a, "--launcher-page") && v) {
            opt.launcher_page = v, i++;
        } else if (!strcmp(a, "--launcher-keys") && v) {
            opt.launcher_keys = v, i++;
        } else if (!strcmp(a, "--overlay-keys") && v) {
            script_parse(v), i++;
        } else if (!strcmp(a, "--overlay-shot") && v) {
            opt.overlay_shot = v, i++;
        } else if ((!strcmp(a, "--shot") || !strcmp(a, "--vram-shot")) && v) {
            const char *colon = strchr(v, ':');
            if (colon && opt.nshots < 64) {
                opt.shots[opt.nshots].frame = atol(v);
                snprintf(opt.shots[opt.nshots].path, 512, "%s", colon + 1);
                opt.shots[opt.nshots].vram = !strcmp(a, "--vram-shot");
                opt.nshots++;
            }
            i++;
        } else if (!strcmp(a, "--record") && v) {
            /* START:COUNT:STEP:PREFIX -> PREFIX_0000.png, ... */
            int n = 0;
            if (sscanf(v, "%ld:%ld:%ld:%n", &opt.record.start, &opt.record.count,
                       &opt.record.step, &n) < 3 || !n || opt.record.step < 1) {
                fprintf(stderr, "bad --record %s (use START:COUNT:STEP:PREFIX)\n", v);
                exit(1);
            }
            snprintf(opt.record.prefix, sizeof opt.record.prefix, "%s", v + n);
            i++;
        } else if (!strcmp(a, "--stick") && v) {
            /* FRAME:LEN:LX:LY:RX:RY (decimal 0-255, 128 = centre) */
            long f, len;
            int lx, ly, rx, ry;
            if (sscanf(v, "%ld:%ld:%d:%d:%d:%d", &f, &len, &lx, &ly, &rx, &ry) == 6 &&
                opt.nsticks < 64)
                opt.sticks[opt.nsticks++] = (typeof(opt.sticks[0])){f, len, lx, ly, rx, ry};
            i++;
        } else if (!strcmp(a, "--dump-ram") && v) {
            const char *colon = strchr(v, ':');
            if (colon && opt.ndumps < 16) {
                opt.dumps[opt.ndumps].frame = atol(v);
                snprintf(opt.dumps[opt.ndumps].path, 512, "%s", colon + 1);
                opt.ndumps++;
            }
            i++;
        } else if (!strcmp(a, "--poke") && v) {
            /* FRAME:ADDR:VALUE[:SIZE] (address and value hex, size 1/2/4) */
            long f;
            unsigned addr, val;
            int size = 4;
            if (sscanf(v, "%ld:%x:%x:%d", &f, &addr, &val, &size) < 3 ||
                (size != 1 && size != 2 && size != 4)) {
                fprintf(stderr, "bad --poke %s (use FRAME:ADDR:VALUE[:SIZE])\n", v);
                exit(1);
            }
            if (opt.npokes < 32)
                opt.pokes[opt.npokes++] = (typeof(opt.pokes[0])){f, addr, val, size};
            i++;
        } else if (!strcmp(a, "--press") && v) {
            /* FRAME:BUTTON[:LEN] */
            char buf[64];
            snprintf(buf, sizeof buf, "%s", v);
            char *f = strtok(buf, ":"), *bn = strtok(NULL, ":"), *ln = strtok(NULL, ":");
            if (f && bn && opt.npresses < 256) {
                opt.presses[opt.npresses].frame = atol(f);
                opt.presses[opt.npresses].sprint = !strcmp(bn, "sprint");
                opt.presses[opt.npresses].button =
                    opt.presses[opt.npresses].sprint ? 0 : button_by_name(bn);
                opt.presses[opt.npresses].len = ln ? atol(ln) : 4;
                opt.npresses++;
            }
            i++;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            exit(1);
        }
    }
}

/* Runtime values from the settings (after the launcher). */
static void apply_settings(const Settings *c) {
    opt.aspect = settings_aspect(c);
    opt.render_scale = c->internal_scale;
    opt.no_pgxp = !c->pgxp;
    opt.no_vsync = !c->vsync;
    opt.smooth = c->smooth;
    opt.show_fps = c->show_fps;
    opt.mouse_sens = c->mouse_sens;
    g_sprint_pct = c->sprint ? c->sprint_pct : 100;
    g_infinite_flashlight = c->infinite_flashlight;
    g_cycle_scale = cycle_scale_for(c->cpu_pct);
    g_enemy_damage_pct = c->enemy_damage_pct;
    memcpy(g_enemy_damage_group_pct, c->enemy_damage_group_pct, sizeof g_enemy_damage_group_pct);
    apply_camera(c);
    if (!opt.exe && c->skip_intro)
        opt.exe = "ALIEN.BIN";
}

int main(int argc, char **argv) {
    /* settings: defaults, then config.ini (interactive runs only, so test
     * runs are reproducible), then command-line overrides */
    settings_defaults(&g_cfg);
    bool headless = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = true;
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc)
            opt.save_dir = argv[++i];
    }
#ifdef _WIN32
    {
        void win32_setup_console(const char *data_dir);
        win32_setup_console(platform_data_dir());
    }
#endif
    if (!headless) {
        settings_load(&g_cfg, settings_path());
    }
    g_saved = g_cfg;
    parse_args(argc, argv);
    if (!g_cfg.disc[0]) {
        /* convenience default: a disc image folder next to the binary's cwd */
        char cue[1024];
        if (disc_resolve("AlienResurrectionBinary", cue, sizeof cue))
            snprintf(g_cfg.disc, sizeof g_cfg.disc, "%s", cue);
    }
    bool launcher = !opt.headless && (!opt.no_launcher || opt.launcher_shot || opt.launcher_page);

    bool want_gpu = !opt.headless || !opt.software || (opt.headless && opt.renderer_set);
    SDL_InitFlags flags = opt.headless ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD);
    if (opt.headless && !opt.renderer_set)
        opt.software = true;
    want_gpu = !opt.headless || !opt.software;
    if (want_gpu)
        flags |= SDL_INIT_VIDEO; /* the GPU device needs the video subsystem */
    if (!SDL_Init(flags)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_CreateDirectory(platform_data_dir());
    if (!opt.headless) {
        float aspect = g_cfg.widescreen ? (float)g_cfg.aspect_w / (float)g_cfg.aspect_h
                                        : 4.0f / 3.0f;
        g_win = SDL_CreateWindow("Alien Resurrection", (int)(240 * opt.scale * aspect),
                                 (int)(240 * opt.scale), SDL_WINDOW_RESIZABLE |
                                 SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!g_win) {
            fprintf(stderr, "window: %s\n", SDL_GetError());
            return 1;
        }
    }
    if (launcher) {
        if (opt.launcher_shot || opt.launcher_page)
            launcher_debug_shot(opt.launcher_shot, opt.launcher_page);
        if (opt.launcher_keys)
            launcher_debug_keys(opt.launcher_keys);
        static char driver[16];
        snprintf(driver, sizeof driver, "%s", g_cfg.gpu_driver);
        g_render_driver = driver;
        if (!launcher_run(g_win, &g_cfg, &g_saved, settings_path()))
            return 0;
    }
    apply_settings(&g_cfg);
    if (!opt.exe)
        opt.exe = "SLUS_006.33";
    char cue[1024] = "";
    if (!g_cfg.disc[0] || !disc_resolve(g_cfg.disc, cue, sizeof cue) || !disc_open(cue)) {
        fprintf(stderr, "Disc image not found%s%s. Choose it in the launcher, or pass it with --disc:\n"
                        "  alien --disc \"/path/to/Alien Resurrection (USA).cue\"\n",
                g_cfg.disc[0] ? ": " : "", g_cfg.disc);
        return 1;
    }

    /* widescreen needs the hardware renderer to keep 2D elements in shape */
    ws_configure(opt.software ? 4.0f / 3.0f : opt.aspect);
    /* precision geometry also needs the hardware renderer */
    g_pgxp = !opt.software && !opt.no_pgxp;
    /* the in-game overlay applies later changes relative to these */
    g_applied = g_cfg;
    g_overlay_info = (OverlayInfo){.software = opt.software,
                                   .running_scale = g_cfg.internal_scale,
                                   .running_vsync = g_cfg.vsync,
                                   .running_skip_intro = g_cfg.skip_intro};
    if (g_win) {
        if (g_cfg.fullscreen)
            SDL_SetWindowFullscreen(g_win, true);
        else if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_MAXIMIZED)) {
            /* the launcher may have changed the aspect ratio */
            int w, h;
            SDL_GetWindowSize(g_win, &w, &h);
            SDL_SetWindowSize(g_win, (int)((float)h * opt.aspect + 0.5f), h);
        }
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, NULL, NULL);
        if (g_audio)
            SDL_ResumeAudioStreamDevice(g_audio);
        else
            LOGW("main", "no audio: %s", SDL_GetError());
        /* a controller that was already connected sends no "added" event */
        int n = 0;
        SDL_JoystickID *pads = SDL_GetGamepads(&n);
        if (g_script.uses_pad)
            g_pad = SDL_OpenGamepad(script_attach_pad());
        else if (n > 0)
            g_pad = SDL_OpenGamepad(pads[0]);
        SDL_free(pads);
    }
    if (g_win && !opt.no_vsync) {
        /* if the display runs at ~60 Hz, present with vsync and let it pace
         * emulation (smoothest); otherwise keep PS1-accurate sleep pacing */
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
        float hz = dm ? dm->refresh_rate : 0.0f;
        g_vsync_paced = hz > 58.5f && hz < 61.5f;
        if (g_vsync_paced)
            g_frame_ns = (uint64_t)(1e9 / hz);
        LOGI("main", "display refresh %.2f Hz: %s", hz,
             g_vsync_paced ? "vsync paced" : "timer paced, vsync presentation");
    }
    if (want_gpu) {
        RenderConfig rc = {.scale = opt.render_scale, .software = opt.software,
                           .headless = opt.headless, .smooth = opt.smooth,
                           .vsync = !opt.no_vsync, .vsync_paced = g_vsync_paced};
        static char driver[16]; /* the API this run uses (the overlay compares) */
        snprintf(driver, sizeof driver, "%s", g_cfg.gpu_driver);
        g_render_driver = driver;
        g_gpu_ok = render_init(g_win, &rc);
        if (!g_gpu_ok && !opt.headless) {
            fprintf(stderr, "GPU initialisation failed\n");
            return 1;
        }
    }
    g_rgb = malloc(1024 * 512 * 3);

    static CPU cpu;
    g_cpu = &cpu;
    cpu.next_event = 0;
    sched_init();
    bios_init(&cpu);
    dma_init();
    timers_init();
    gpu_init();
    cdrom_init();
    spu_init();
    mdec_init();
    sio_init();
    sched_register(EV_VBLANK, vblank_event);
    sched_at(EV_VBLANK, FRAME_CYCLES);
    cpu.next_event = sched_next();

    bios_boot(&cpu, opt.exe);
    LOGI("main", "program exited");
    return 0;
}
