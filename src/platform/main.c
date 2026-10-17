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
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../hw/gpu.h"
#include "../render/render.h"
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
    struct { long frame; char path[512]; } dumps[16];
    int ndumps;
    struct { long frame; long len; uint16_t button; bool sprint; } presses[256];
    int npresses;
    struct { long frame; long len; int lx, ly, rx, ry; } sticks[64];
    int nsticks;
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

/* mouse look: relative motion accumulated between frames drives the right
 * analog stick (the game's look/turn input) */
static float g_mouse_dx, g_mouse_dy;
static bool g_mouse_captured;
static uint32_t g_mouse_buttons;

static uint8_t stick_add(uint8_t base, float delta) {
    float v = (float)base + delta;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static void poll_input(void) {
    bool sprint = false;
    uint16_t b = scripted_buttons(&sprint);
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
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
        b |= in.buttons;
        if (in.lx != 0x80) lx = in.lx;
        if (in.ly != 0x80) ly = in.ly;
        if (in.rx != 0x80) rx = in.rx;
        if (in.ry != 0x80) ry = in.ry;
        sprint |= in.sprint;
        g_turbo = in.fast_forward;
        if (g_mouse_captured && g_cfg.mouse_look) {
            rx = stick_add(rx, g_mouse_dx * opt.mouse_sens);
            ry = stick_add(ry, g_mouse_dy * opt.mouse_sens);
        }
        g_mouse_dx = g_mouse_dy = 0;
    }
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
            if (n && dir[n - 1] == '/')
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

void platform_vblank(void) {
    g_frame++;
    if (opt.show_fps)
        fps_tick();
    for (int i = 0; i < opt.nshots; i++)
        if (opt.shots[i].frame == g_frame)
            write_shot(opt.shots[i].path, opt.shots[i].vram);
    for (int i = 0; i < opt.ndumps; i++)
        if (opt.dumps[i].frame == g_frame) {
            FILE *f = fopen(opt.dumps[i].path, "wb");
            if (f) {
                fwrite(g_ram, 1, sizeof g_ram, f);
                fclose(f);
                LOGI("main", "frame %ld: dumped RAM to %s", g_frame, opt.dumps[i].path);
            }
        }
    if (opt.max_frames >= 0 && g_frame >= opt.max_frames) {
        LOGI("main", "reached frame limit %ld", g_frame);
        exit(0);
    }
    if (opt.headless) {
        poll_input();
        return;
    }
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
                   "  --cpu-speed PCT emulated CPU speed in percent (default 300; 100 = original)\n"
                   "  --sprint-speed PCT  movement speed while Shift is held (default 175; 100 = off)\n"
                   "  --mouse-sens N  mouse-look sensitivity (default 6)\n"
                   "  --save-dir DIR  folder for the memory card images\n"
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
        } else if (!strcmp(a, "--no-pgxp")) {
            g_cfg.pgxp = false;
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
        } else if ((!strcmp(a, "--shot") || !strcmp(a, "--vram-shot")) && v) {
            const char *colon = strchr(v, ':');
            if (colon && opt.nshots < 64) {
                opt.shots[opt.nshots].frame = atol(v);
                snprintf(opt.shots[opt.nshots].path, 512, "%s", colon + 1);
                opt.shots[opt.nshots].vram = !strcmp(a, "--vram-shot");
                opt.nshots++;
            }
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
    opt.aspect = c->widescreen ? (float)c->aspect_w / (float)c->aspect_h : 4.0f / 3.0f;
    opt.render_scale = c->internal_scale;
    opt.no_pgxp = !c->pgxp;
    opt.no_vsync = !c->vsync;
    opt.smooth = c->smooth;
    opt.show_fps = c->show_fps;
    opt.mouse_sens = c->mouse_sens;
    g_sprint_pct = c->sprint ? c->sprint_pct : 100;
    g_cycle_scale = (uint32_t)(32 * 100 / (c->cpu_pct < 50 ? 50 : c->cpu_pct));
    if (g_cycle_scale < 1)
        g_cycle_scale = 1;
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
    static Settings saved;
    if (!headless) {
        settings_load(&g_cfg, settings_path());
    }
    saved = g_cfg;
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
    mkdir(platform_data_dir(), 0755);
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
        if (!launcher_run(g_win, &g_cfg, &saved, settings_path()))
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
        if (n > 0)
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
