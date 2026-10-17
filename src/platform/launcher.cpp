/*
 * Start-up launcher (Dear ImGui, SDL GPU backend).
 *
 * Main menu over title art read from the player's own disc (a loading
 * screen from PACK.BIN), plus Settings and Controls pages styled after the
 * game's own option screens: "OFF  ON" toggles, left/right choices and
 * segmented sliders. Works with mouse, keyboard and controller.
 *
 * The same pages also form the in-game overlay (F2 / Select+Start): a pause
 * menu drawn over the frozen game image with the game renderer's GPU
 * device, where settings apply while playing.
 */
#include "launcher.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../../third_party/imgui/backends/imgui_impl_sdl3.h"
#include "../../third_party/imgui/backends/imgui_impl_sdlgpu3.h"
#include "discinfo.h"

extern "C" int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);
extern "C" void disc_close(void);

namespace {

/* ---- palette (the game's front end: grey text, green glow) ---- */
const ImU32 C_BG = IM_COL32(6, 8, 8, 255);
const ImU32 C_TEXT = IM_COL32(205, 214, 208, 255);
const ImU32 C_DIM = IM_COL32(110, 122, 115, 255);
const ImU32 C_FAINT = IM_COL32(62, 70, 66, 255);
const ImU32 C_HOT = IM_COL32(236, 255, 240, 255);
const ImU32 C_ACCENT = IM_COL32(104, 230, 120, 255);
const ImU32 C_WARN = IM_COL32(232, 176, 70, 255);
const ImU32 C_PANEL = IM_COL32(4, 7, 6, 222);

enum Page { PAGE_MAIN, PAGE_SETTINGS, PAGE_CONTROLS, PAGE_QUIT };

/* Keyboard/controller navigation of the Settings and Controls pages: the
 * items registered last frame form rows (and columns in the bindings
 * table); Up/Down move between rows, Left/Right between columns or change
 * the value of a single-item row. */
struct NavItem {
    int row, col;
    bool disabled;
};

struct Capture {
    int action = -1;
    int column = 0;     /* 0,1 = keyboard slots, 2 = controller */
    Uint64 started = 0;
};

struct Launcher {
    SDL_Window *win = nullptr;
    SDL_GPUDevice *dev = nullptr;
    Settings *cfg = nullptr, *saved = nullptr;
    const char *path = nullptr;
    bool dirty = false;

    ImFont *f_title = nullptr, *f_label = nullptr, *f_body = nullptr;
    float U = 1.0f;     /* UI unit: window height / 720 */
    ImVec2 S;           /* window size (points) */

    Page page = PAGE_MAIN;
    int menu_sel = 0;
    const char *desc = nullptr;

    std::vector<NavItem> nav_items, nav_prev;
    int nav_sel = 0, nav_row = 0;
    bool nav_kb = false, nav_scroll = false;
    bool in_accept = false, in_left = false, in_right = false;

    /* disc */
    DiscStatus disc = DISC_NONE;
    char disc_msg[256] = "";
    SDL_GPUTexture *art = nullptr;
    int art_w = 0, art_h = 0;
    std::mutex pick_mu;
    std::string picked;
    bool pick_ready = false, pick_open = false;

    Capture cap;
    int result = -1;    /* 1 = start, 0 = quit */

    /* --launcher-shot: render a page offscreen to a PNG, then quit */
    const char *shot_path = nullptr;
    int shot_frames = 0;
    bool auto_start = false;  /* --launcher-page start: press START GAME */
    std::vector<SDL_Scancode> script;  /* --launcher-keys: synthetic key presses */
    size_t script_pos = 0;
    int script_tick = 0;

    /* in-game overlay */
    bool ingame = false;
    bool imgui_ready = false;   /* the overlay's ImGui context exists */
    OverlayInfo info = {};
    int overlay_result = OVERLAY_OPEN;
    int quit_sel = 0;
    /* controller buttons held when the overlay opened (e.g. Select+Start)
     * are ignored until every button has been released */
    bool pad_blocked = false;
    const char *tag = nullptr;  /* note drawn at the right of the next row */
    bool tag_warn = false;
};

Launcher L;

/* ---- settings persistence: every edit goes to both copies ---- */
template <class T> void mark(T *field) {
    size_t off = (size_t)((char *)field - (char *)L.cfg);
    memcpy((char *)L.saved + off, field, sizeof(T));
    L.dirty = true;
}

void save_if_dirty() {
    if (!L.dirty)
        return;
    if (!settings_save(L.saved, L.path))
        SDL_Log("launcher: cannot write %s", L.path);
    L.dirty = false;
}

/* ---- GPU helpers ---- */
SDL_GPUTexture *upload_rgba(const uint8_t *px, int w, int h) {
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(L.dev, &ti);
    if (!t)
        return nullptr;
    SDL_GPUTransferBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(L.dev, &bi);
    void *dst = SDL_MapGPUTransferBuffer(L.dev, tb, false);
    memcpy(dst, px, (size_t)w * (size_t)h * 4);
    SDL_UnmapGPUTransferBuffer(L.dev, tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(L.dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion reg = {};
    reg.texture = t;
    reg.w = (Uint32)w;
    reg.h = (Uint32)h;
    reg.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &reg, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(L.dev, tb);
    return t;
}

/* ---- disc ---- */
void check_disc() {
    char cue[1024] = "";
    L.disc = disc_check(L.cfg->disc, cue, sizeof cue, L.disc_msg, sizeof L.disc_msg);
    if (L.art) {
        SDL_WaitForGPUIdle(L.dev);
        SDL_ReleaseGPUTexture(L.dev, L.art);
        L.art = nullptr;
    }
    if (L.disc == DISC_OK) {
        if (strcmp(cue, L.cfg->disc) || strcmp(cue, L.saved->disc)) {
            snprintf(L.cfg->disc, sizeof L.cfg->disc, "%s", cue);
            mark(&L.cfg->disc);
        }
        int w, h;
        uint8_t *px = disc_title_art(&w, &h);
        if (px) {
            L.art = upload_rgba(px, w, h);
            L.art_w = w, L.art_h = h;
            free(px);
        }
        disc_close();
    }
}

void SDLCALL on_pick(void *, const char *const *files, int) {
    std::lock_guard<std::mutex> lk(L.pick_mu);
    L.pick_open = false;
    if (files && files[0]) {
        L.picked = files[0];
        L.pick_ready = true;
    }
}

void open_disc_dialog() {
    if (L.pick_open)
        return;
    static const SDL_DialogFileFilter filters[] = {
        {"PlayStation disc image (.cue, .bin)", "cue;bin"},
        {"All files", "*"},
    };
    std::string dir;
    const char *slash = strrchr(L.cfg->disc, '/');
    if (slash)
        dir.assign(L.cfg->disc, (size_t)(slash - L.cfg->disc));
    L.pick_open = true;
    SDL_ShowOpenFileDialog(on_pick, nullptr, L.win, filters, 2,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

void poll_picked() {
    std::string p;
    {
        std::lock_guard<std::mutex> lk(L.pick_mu);
        if (!L.pick_ready)
            return;
        p = L.picked;
        L.pick_ready = false;
    }
    snprintf(L.cfg->disc, sizeof L.cfg->disc, "%s", p.c_str());
    mark(&L.cfg->disc);
    check_disc();
    save_if_dirty();
}

/* ---- drawing helpers ---- */
ImVec2 text_size(ImFont *f, float size, const char *s) {
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
}

/* letter-spaced text centred on cx */
void text_spaced(ImDrawList *dl, ImFont *f, float size, float cx, float y, ImU32 col,
                 const char *s, float spacing) {
    float w = 0;
    for (const char *p = s; *p; p++)
        w += text_size(f, size, std::string(p, 1).c_str()).x + (p[1] ? spacing : 0);
    float x = cx - w * 0.5f;
    for (const char *p = s; *p; p++) {
        char c[2] = {*p, 0};
        dl->AddText(f, size, ImVec2(x, y), col, c);
        x += text_size(f, size, c).x + spacing;
    }
}

/* horizontal glow bar (the game's selection highlight) */
void glow_bar(ImDrawList *dl, float cx, float y, float half_w, float h, ImU32 col, float alpha) {
    ImU32 c = (col & 0x00FFFFFF) | ((ImU32)(alpha * 255.0f) << 24);
    ImU32 z = col & 0x00FFFFFF;
    dl->AddRectFilledMultiColor(ImVec2(cx - half_w, y), ImVec2(cx, y + h), z, c, c, z);
    dl->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + half_w, y + h), c, z, z, c);
}

void header(ImDrawList *dl, const char *title, float cx, float y) {
    float s = 30 * L.U;
    glow_bar(dl, cx, y + s * 0.30f, 170 * L.U, s * 0.42f, C_ACCENT, 0.55f);
    glow_bar(dl, cx, y + s * 0.47f, 240 * L.U, 1.5f * L.U, C_ACCENT, 0.9f);
    text_spaced(dl, L.f_title, s, cx, y, C_HOT, title, 3 * L.U);
}

void background(ImDrawList *dl) {
    const ImVec2 S = L.S;
    dl->AddRectFilled(ImVec2(0, 0), S, C_BG);
    if (L.art) {
        /* cover the window, but keep at least 75% of the art's width (the
         * logo) visible; the art fades into the background colour */
        float sc = fmaxf(S.x / L.art_w, S.y / L.art_h);
        sc = fminf(sc, S.x / (0.75f * L.art_w));
        float w = L.art_w * sc, h = L.art_h * sc;
        ImVec2 p0((S.x - w) * 0.5f, (S.y - h) * 0.5f - S.y * 0.08f);
        dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)L.art), p0, ImVec2(p0.x + w, p0.y + h));
        /* hide the "uploading data" caption under the logo */
        float y0 = p0.y + h * 0.565f, y1 = p0.y + h * 0.605f;
        dl->AddRectFilledMultiColor(ImVec2(0, y0), ImVec2(S.x, y1), C_BG & 0x00FFFFFF,
                                    C_BG & 0x00FFFFFF, C_BG, C_BG);
        dl->AddRectFilled(ImVec2(0, y1), S, C_BG);
        /* soften the edges */
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(S.x, p0.y + h * 0.12f), C_BG,
                                    C_BG, C_BG & 0x00FFFFFF, C_BG & 0x00FFFFFF);
        if (p0.x > -1.0f) {
            float e = 0.10f * w;
            dl->AddRectFilledMultiColor(ImVec2(p0.x, 0), ImVec2(p0.x + e, S.y), C_BG,
                                        C_BG & 0x00FFFFFF, C_BG & 0x00FFFFFF, C_BG);
            dl->AddRectFilledMultiColor(ImVec2(p0.x + w - e, 0), ImVec2(p0.x + w, S.y),
                                        C_BG & 0x00FFFFFF, C_BG, C_BG, C_BG & 0x00FFFFFF);
        }
    } else {
        /* no disc yet: a plain rendition of the title */
        float cx = S.x * 0.5f, y = S.y * 0.22f;
        float s1 = S.y * 0.17f;
        glow_bar(dl, cx, y + s1 * 0.93f, S.x * 0.40f, S.y * 0.035f, C_ACCENT, 0.45f);
        glow_bar(dl, cx, y + s1 * 1.04f, S.x * 0.45f, 2.0f * L.U, C_ACCENT, 0.9f);
        text_spaced(dl, L.f_title, s1, cx, y, C_TEXT, "ALIEN", s1 * 0.10f);
        text_spaced(dl, L.f_title, s1 * 0.30f, cx, y + s1 * 1.02f, C_HOT, "RESURRECTION",
                    s1 * 0.13f);
    }
}

/* ---- main menu ---- */
/* Key press for navigation; controller buttons are ignored while blocked. */
bool pressed(ImGuiKey k, bool repeat = true) {
    if (L.pad_blocked && ImGui::IsGamepadKey(k))
        return false;
    return ImGui::IsKeyPressed(k, repeat);
}

bool nav_up() {
    return pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp) ||
           pressed(ImGuiKey_GamepadLStickUp);
}
bool nav_down() {
    return pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown) ||
           pressed(ImGuiKey_GamepadLStickDown);
}
bool nav_accept() {
    return pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false) ||
           pressed(ImGuiKey_Space, false) || pressed(ImGuiKey_GamepadFaceDown, false);
}
bool nav_back() {
    return pressed(ImGuiKey_Escape, false) || pressed(ImGuiKey_GamepadFaceRight, false);
}

void open_page(Page p) {
    save_if_dirty();
    L.page = p;
    L.nav_sel = 0;
    L.nav_kb = false;
    L.nav_items.clear();
    L.nav_prev.clear();
}

bool key_left() {
    return pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft) ||
           pressed(ImGuiKey_GamepadLStickLeft);
}
bool key_right() {
    return pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight) ||
           pressed(ImGuiKey_GamepadLStickRight);
}

void nav_begin() {
    L.nav_prev.swap(L.nav_items);
    L.nav_items.clear();
    L.nav_row = 0;
    L.in_left = L.in_right = L.in_accept = false;
    const ImGuiIO &io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)
        L.nav_kb = false;
    if (L.cap.action >= 0)
        return;
    const std::vector<NavItem> &P = L.nav_prev;
    if (P.empty())
        return;
    bool up = nav_up(), down = nav_down(), left = key_left(), right = key_right();
    bool accept = nav_accept();
    if (!(up || down || left || right || accept))
        return;
    L.nav_kb = true;
    int cur = L.nav_sel < (int)P.size() ? L.nav_sel : 0;
    if (P[cur].disabled) {
        for (int i = 0; i < (int)P.size(); i++)
            if (!P[i].disabled) {
                cur = i;
                break;
            }
    }
    auto row_items = [&](int row) {
        int n = 0;
        for (const NavItem &it : P)
            n += it.row == row;
        return n;
    };
    if (up || down) {
        int dir = down ? 1 : -1;
        /* rows are registered in increasing order */
        int best = -1;
        for (int i = cur + dir; i >= 0 && i < (int)P.size(); i += dir) {
            if (P[i].row == P[cur].row || P[i].disabled)
                continue;
            int row = P[i].row, bd = 1 << 30;
            for (int j = 0; j < (int)P.size(); j++)
                if (P[j].row == row && !P[j].disabled && abs(P[j].col - P[cur].col) < bd)
                    bd = abs(P[j].col - P[cur].col), best = j;
            break;
        }
        if (best >= 0) {
            cur = best;
            L.nav_scroll = true;
        }
    } else if ((left || right) && row_items(P[cur].row) > 1) {
        int dir = right ? 1 : -1;
        for (int i = cur + dir; i >= 0 && i < (int)P.size() && P[i].row == P[cur].row; i += dir)
            if (!P[i].disabled) {
                cur = i;
                break;
            }
    } else {
        L.in_left = left;
        L.in_right = right;
    }
    L.in_accept = accept;
    L.nav_sel = cur;
}

/* Register a navigable item; returns its index for this frame. */
int nav_add(int row, int col, bool disabled) {
    L.nav_items.push_back(NavItem{row, col, disabled});
    return (int)L.nav_items.size() - 1;
}

bool nav_is(int idx) { return L.nav_kb && idx == L.nav_sel; }

/* after the item's InvisibleButton: hover tracking and scrolling */
void nav_item_post(int idx) {
    const ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0))
        L.nav_sel = idx;
    if (nav_is(idx) && L.nav_scroll) {
        ImGui::SetScrollHereY(0.5f);
        L.nav_scroll = false;
    }
}

/* Vertical menu in the style of the game's front end, starting at y0.
 * Returns the item activated this frame (keyboard, controller or click),
 * or -1. Items flagged in `dim` are drawn faint but can still be chosen. */
int menu(ImDrawList *dl, const char *const *items, int n, int *sel, float y0, const bool *dim) {
    float size = 34 * L.U, step = size * 1.38f, cx = L.S.x * 0.5f;
    if (nav_up()) *sel = (*sel + n - 1) % n;
    if (nav_down()) *sel = (*sel + 1) % n;
    int activate = nav_accept() ? *sel : -1;

    const ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < n; i++) {
        float y = y0 + i * step;
        ImVec2 ts = text_size(L.f_title, size, items[i]);
        float spacing = 4 * L.U, w = ts.x + spacing * (float)strlen(items[i]);
        ImRect r(ImVec2(cx - w * 0.5f - 30 * L.U, y - 4 * L.U),
                 ImVec2(cx + w * 0.5f + 30 * L.U, y + size));
        if (r.Contains(io.MousePos) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0))
            *sel = i;
        if (r.Contains(io.MousePos) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            activate = i;
        bool s = *sel == i;
        bool faint = dim && dim[i];
        if (s) {
            glow_bar(dl, cx, y + size * 0.20f, w * 0.75f + 60 * L.U, size * 0.55f, C_ACCENT, 0.50f);
            glow_bar(dl, cx, y + size * 0.46f, w * 0.85f + 80 * L.U, 1.5f * L.U, C_ACCENT, 0.9f);
        }
        text_spaced(dl, L.f_title, size, cx, y, faint ? C_FAINT : s ? C_HOT : C_TEXT, items[i],
                    spacing);
    }
    return activate;
}

void draw_main(ImDrawList *dl) {
    enum { M_START, M_DISC, M_SETTINGS, M_CONTROLS, M_QUIT, M_COUNT };
    static const char *const items[M_COUNT] = {"START GAME", "SELECT DISC IMAGE", "SETTINGS",
                                               "CONTROLS", "QUIT"};
    const bool ok = L.disc == DISC_OK;
    float step = 34 * L.U * 1.38f;
    float y0 = L.S.y * 0.60f;
    if (L.S.y * 0.60f + step * M_COUNT > L.S.y * 0.90f)
        y0 = L.S.y * 0.90f - step * M_COUNT;
    const bool dim[M_COUNT] = {!ok, false, false, false, false};
    int activate = menu(dl, items, M_COUNT, &L.menu_sel, y0, dim);
    switch (activate) {
    case M_START:
        if (ok) {
            save_if_dirty();
            L.result = 1;
        } else {
            open_disc_dialog();
        }
        break;
    case M_DISC: open_disc_dialog(); break;
    case M_SETTINGS: open_page(PAGE_SETTINGS); break;
    case M_CONTROLS: open_page(PAGE_CONTROLS); break;
    case M_QUIT: L.result = 0; break;
    default: break;
    }

    /* footer: disc status, controller, version */
    float fs = 17 * L.U, fy = L.S.y - fs - 18 * L.U, mx = 28 * L.U;
    ImU32 dc = ok ? C_ACCENT : (L.disc == DISC_NONE ? C_DIM : C_WARN);
    dl->AddCircleFilled(ImVec2(mx + 5 * L.U, fy + fs * 0.5f), 4.5f * L.U, dc);
    char line[400];
    snprintf(line, sizeof line, "%s", L.disc == DISC_NONE
                                          ? "No disc image selected - choose your Alien Resurrection (USA) .cue file."
                                          : L.disc_msg);
    dl->AddText(L.f_body, fs, ImVec2(mx + 18 * L.U, fy), ok ? C_TEXT : dc, line);
    if (ok) {
        const char *base = strrchr(L.cfg->disc, '/');
        dl->AddText(L.f_body, fs * 0.85f, ImVec2(mx + 18 * L.U, fy - fs * 1.1f), C_DIM,
                    base ? base + 1 : L.cfg->disc);
    }
    int n = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&n);
    char ctl[160];
    if (n > 0) {
        const char *name = SDL_GetGamepadNameForID(pads[0]);
        snprintf(ctl, sizeof ctl, "Controller: %s", name ? name : "connected");
    } else {
        snprintf(ctl, sizeof ctl, "Keyboard & mouse");
    }
    SDL_free(pads);
    char ver[64];
    snprintf(ver, sizeof ver, "Version %s", PORT_VERSION);
    float vw = text_size(L.f_body, fs, ver).x;
    dl->AddText(L.f_body, fs, ImVec2(L.S.x - mx - vw, fy), C_DIM, ver);
    float cw = text_size(L.f_body, fs, ctl).x;
    dl->AddText(L.f_body, fs, ImVec2(L.S.x - mx - cw, fy - fs * 1.25f), C_DIM, ctl);
}

/* ---- in-game overlay: pause menu ---- */
/* dark framed panel (as on the Settings page) behind a centred menu, so the
 * game image underneath cannot compete with the text */
void menu_panel(ImDrawList *dl, float y0, float y1, float w) {
    ImVec2 p0(L.S.x * 0.5f - w * 0.5f, y0), p1(L.S.x * 0.5f + w * 0.5f, y1);
    dl->AddRectFilled(p0, p1, IM_COL32(4, 7, 6, 240), 6 * L.U);
    dl->AddRect(p0, p1, IM_COL32(104, 230, 120, 60), 6 * L.U, 0, 1.0f * L.U);
}

void draw_pause(ImDrawList *dl) {
    enum { P_RESUME, P_SETTINGS, P_CONTROLS, P_QUIT, P_COUNT };
    static const char *const items[P_COUNT] = {"RESUME", "SETTINGS", "CONTROLS",
                                               "QUIT TO DESKTOP"};
    float step = 34 * L.U * 1.38f;
    float y0 = L.S.y * 0.5f - step * P_COUNT * 0.5f + 30 * L.U;
    menu_panel(dl, y0 - 128 * L.U, y0 + step * P_COUNT + 14 * L.U, 560 * L.U);
    header(dl, "PAUSED", L.S.x * 0.5f, y0 - 92 * L.U);
    int activate = menu(dl, items, P_COUNT, &L.menu_sel, y0, nullptr);
    if (nav_back())
        activate = P_RESUME;
    switch (activate) {
    case P_RESUME: L.overlay_result = OVERLAY_RESUME; break;
    case P_SETTINGS: open_page(PAGE_SETTINGS); break;
    case P_CONTROLS: open_page(PAGE_CONTROLS); break;
    case P_QUIT:
        L.quit_sel = 0;
        L.page = PAGE_QUIT;
        break;
    default: break;
    }

    float fs = 17 * L.U, fy = L.S.y - fs - 18 * L.U, mx = 28 * L.U;
    dl->AddText(L.f_body, fs, ImVec2(mx, fy), C_DIM,
                "F2 or Select + Start on a controller: resume");
    char ver[64];
    snprintf(ver, sizeof ver, "Version %s", PORT_VERSION);
    float vw = text_size(L.f_body, fs, ver).x;
    dl->AddText(L.f_body, fs, ImVec2(L.S.x - mx - vw, fy), C_DIM, ver);
}

void draw_quit(ImDrawList *dl) {
    static const char *const items[2] = {"CANCEL", "QUIT TO DESKTOP"};
    float y = L.S.y * 0.5f - 120 * L.U;
    menu_panel(dl, y - 36 * L.U, y + 120 * L.U + 34 * L.U * 1.38f * 2 + 14 * L.U, 640 * L.U);
    header(dl, "QUIT TO DESKTOP?", L.S.x * 0.5f, y);
    const char *m = "Progress since your last save will be lost.";
    float fs = 20 * L.U;
    float mw = text_size(L.f_body, fs, m).x;
    dl->AddText(L.f_body, fs, ImVec2(L.S.x * 0.5f - mw * 0.5f, y + 58 * L.U), C_TEXT, m);
    int activate = menu(dl, items, 2, &L.quit_sel, y + 120 * L.U, nullptr);
    if (nav_back())
        activate = 0;
    if (activate == 0)
        L.page = PAGE_MAIN;
    else if (activate == 1)
        L.overlay_result = OVERLAY_QUIT;
}

/* ---- option rows ---- */
struct Row {
    bool hot = false, pressed = false, left = false, right = false;
    ImRect r;
};

Row row(const char *label, const char *desc, float h = 0) {
    Row o;
    if (h <= 0)
        h = 34 * L.U;
    ImGuiWindow *w = ImGui::GetCurrentWindow();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    int idx = nav_add(L.nav_row++, 0, disabled);
    ImGui::PushID(label);
    o.pressed = ImGui::InvisibleButton("row", ImVec2(width, h));
    nav_item_post(idx);
    o.hot = (ImGui::IsItemHovered() && !L.nav_kb) || nav_is(idx);
    if (nav_is(idx)) {
        o.pressed |= L.in_accept;
        o.left = L.in_left;
        o.right = L.in_right;
    }
    ImGui::PopID();
    o.r = ImRect(p, ImVec2(p.x + width, p.y + h));
    if (disabled)
        o.hot = o.pressed = o.left = o.right = false;
    ImDrawList *dl = w->DrawList;
    if (o.hot) {
        dl->AddRectFilledMultiColor(o.r.Min, o.r.Max, IM_COL32(104, 230, 120, 34),
                                    IM_COL32(104, 230, 120, 6), IM_COL32(104, 230, 120, 6),
                                    IM_COL32(104, 230, 120, 34));
        dl->AddRectFilled(o.r.Min, ImVec2(o.r.Min.x + 3 * L.U, o.r.Max.y), C_ACCENT);
        L.desc = desc;
    }
    float fs = 21 * L.U;
    dl->AddText(L.f_label, fs, ImVec2(o.r.Min.x + 16 * L.U, o.r.Min.y + (h - fs) * 0.5f),
                disabled ? C_FAINT : o.hot ? C_HOT : C_TEXT, label);
    if (L.tag) {
        float ts = 14 * L.U;
        float tw = text_size(L.f_label, ts, L.tag).x;
        dl->AddText(L.f_label, ts, ImVec2(o.r.Max.x - 16 * L.U - tw, o.r.Min.y + (h - ts) * 0.5f),
                    L.tag_warn ? C_WARN : C_DIM, L.tag);
        L.tag = nullptr;
    }
    return o;
}

/* In game: the next row only takes effect when the game is started again
 * (highlighted once its value differs from the running one). */
void restart_tag(bool pending) {
    if (L.ingame) {
        L.tag = "APPLIES ON NEXT START";
        L.tag_warn = pending;
    }
}

/* In game with the software renderer: the next row has no effect. */
void hw_only_tag() {
    if (L.ingame && L.info.software) {
        L.tag = "NEEDS HARDWARE RENDERER";
        L.tag_warn = false;
    }
}

float value_x(const Row &r) { return r.r.Min.x + r.r.GetWidth() * 0.60f; }

bool toggle(const char *label, bool *v, const char *desc) {
    Row r = row(label, desc);
    bool nv = *v;
    if (r.pressed) nv = !nv;
    if (r.left) nv = false;
    if (r.right) nv = true;
    bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float fs = 21 * L.U, y = r.r.Min.y + (r.r.GetHeight() - fs) * 0.5f, x = value_x(r);
    ImU32 on = disabled ? C_FAINT : r.hot ? C_ACCENT : C_HOT;
    ImU32 off = disabled ? IM_COL32(40, 46, 43, 255) : C_FAINT;
    dl->AddText(L.f_label, fs, ImVec2(x, y), !nv ? on : off, "OFF");
    dl->AddText(L.f_label, fs, ImVec2(x + 70 * L.U, y), nv ? on : off, "ON");
    if (nv != *v) {
        *v = nv;
        return true;
    }
    return false;
}

bool choice(const char *label, int *idx, const char *const *opts, int n, const char *desc) {
    Row r = row(label, desc);
    int ni = *idx;
    if (r.pressed) {
        bool prev = !L.in_accept && ImGui::GetIO().MousePos.x < value_x(r) + 30 * L.U;
        ni = prev ? ni - 1 : ni + 1;
    }
    if (r.left) ni--;
    if (r.right) ni++;
    ni = (ni % n + n) % n;
    bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float fs = 21 * L.U, y = r.r.Min.y + (r.r.GetHeight() - fs) * 0.5f, x = value_x(r);
    ImU32 c = disabled ? C_FAINT : r.hot ? C_ACCENT : C_HOT;
    ImU32 a = disabled ? IM_COL32(40, 46, 43, 255) : r.hot ? C_ACCENT : C_DIM;
    dl->AddText(L.f_label, fs, ImVec2(x, y), a, "<");
    dl->AddText(L.f_label, fs, ImVec2(x + 30 * L.U, y), c, opts[ni]);
    float tw = text_size(L.f_label, fs, opts[ni]).x;
    dl->AddText(L.f_label, fs, ImVec2(x + 44 * L.U + tw, y), a, ">");
    if (ni != *idx) {
        *idx = ni;
        return true;
    }
    return false;
}

bool slider(const char *label, float *v, float lo, float hi, float step, const char *fmt,
            const char *desc) {
    Row r = row(label, desc);
    float nv = *v;
    if (r.left) nv -= step;
    if (r.right) nv += step;
    bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const int segs = 20;
    float x0 = value_x(r), seg_w = 11 * L.U, gap = 3 * L.U, bw = segs * (seg_w + gap);
    float h = r.r.GetHeight() * 0.42f, y = r.r.Min.y + (r.r.GetHeight() - h) * 0.5f;
    if (!disabled && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= x0 - 8 * L.U && mx <= x0 + bw + 8 * L.U) {
            float t = (mx - x0) / bw;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            nv = lo + roundf(t * (hi - lo) / step) * step;
        }
    }
    nv = nv < lo ? lo : nv > hi ? hi : nv;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    int lit = (int)lroundf((nv - lo) / (hi - lo) * segs);
    for (int i = 0; i < segs; i++) {
        float sx = x0 + i * (seg_w + gap);
        ImU32 c = i < lit ? (disabled ? C_FAINT : r.hot ? C_ACCENT : C_TEXT)
                          : IM_COL32(36, 42, 39, 255);
        dl->AddRectFilled(ImVec2(sx, y), ImVec2(sx + seg_w, y + h), c);
    }
    char buf[32];
    snprintf(buf, sizeof buf, fmt, (double)nv);
    float fs = 19 * L.U;
    dl->AddText(L.f_label, fs, ImVec2(x0 + bw + 14 * L.U, r.r.Min.y + (r.r.GetHeight() - fs) * 0.5f),
                disabled ? C_FAINT : r.hot ? C_ACCENT : C_TEXT, buf);
    if (nv != *v) {
        *v = nv;
        return true;
    }
    return false;
}

void section(const char *title) {
    ImGui::Dummy(ImVec2(0, 10 * L.U));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = 18 * L.U;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(L.f_label, fs, ImVec2(p.x + 16 * L.U, p.y), C_ACCENT, title);
    float tw = text_size(L.f_label, fs, title).x;
    float ly = p.y + fs * 0.55f;
    dl->AddLine(ImVec2(p.x + 28 * L.U + tw, ly), ImVec2(p.x + ImGui::GetContentRegionAvail().x, ly),
                IM_COL32(104, 230, 120, 70), 1.0f * L.U);
    ImGui::Dummy(ImVec2(0, fs + 6 * L.U));
}

/* text button in the footer of a page */
bool footer_button(const char *label, int col, float cx, float y) {
    float fs = 24 * L.U;
    ImVec2 ts = text_size(L.f_title, fs, label);
    ImVec2 p(cx - ts.x * 0.5f - 16 * L.U, y);
    int idx = nav_add(1 << 20, col, false);
    ImGui::SetCursorScreenPos(p);
    bool pressed = ImGui::InvisibleButton(label, ImVec2(ts.x + 32 * L.U, fs + 8 * L.U));
    nav_item_post(idx);
    bool hot = (ImGui::IsItemHovered() && !L.nav_kb) || nav_is(idx);
    pressed |= nav_is(idx) && L.in_accept;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot)
        glow_bar(dl, cx, y + fs * 0.25f, ts.x * 0.8f + 30 * L.U, fs * 0.5f, C_ACCENT, 0.5f);
    dl->AddText(L.f_title, fs, ImVec2(cx - ts.x * 0.5f, y + 2 * L.U), hot ? C_HOT : C_TEXT, label);
    return pressed;
}

/* panel frame shared by the Settings and Controls pages; returns content rect */
ImRect panel(ImDrawList *dl, const char *title) {
    float w = fminf(L.S.x - 60 * L.U, 1000 * L.U);
    ImVec2 p0((L.S.x - w) * 0.5f, 92 * L.U), p1(p0.x + w, L.S.y - 76 * L.U);
    header(dl, title, L.S.x * 0.5f, 30 * L.U);
    dl->AddRectFilled(p0, p1, C_PANEL, 6 * L.U);
    dl->AddRect(p0, p1, IM_COL32(104, 230, 120, 60), 6 * L.U, 0, 1.0f * L.U);
    return ImRect(ImVec2(p0.x + 18 * L.U, p0.y + 10 * L.U), ImVec2(p1.x - 18 * L.U, p1.y - 10 * L.U));
}

void description(ImDrawList *dl, const ImRect &r) {
    if (!L.desc)
        return;
    float fs = 18 * L.U;
    dl->AddText(L.f_body, fs, ImVec2(r.Min.x + 16 * L.U, r.Max.y - fs * 2.6f), C_DIM, L.desc,
                nullptr, r.GetWidth() - 32 * L.U);
}

/* ---- settings page ---- */
void draw_settings(ImDrawList *dl) {
    Settings *c = L.cfg;
    nav_begin();
    ImRect cr = panel(dl, "SETTINGS");
    float desc_h = 64 * L.U;
    ImGui::SetCursorScreenPos(cr.Min);
    ImGui::BeginChild("rows", ImVec2(cr.GetWidth(), cr.GetHeight() - desc_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    L.desc = nullptr;
    const bool ig = L.ingame;
    const bool hw_only = ig && L.info.software;

    section("GRAPHICS");
    ImGui::BeginDisabled(hw_only);
    hw_only_tag();
    if (toggle("Widescreen", &c->widescreen,
               "Wider field of view in 3D. Menus, the HUD, loading screens and movies stay 4:3."))
        mark(&c->widescreen);
    static const char *const aspects[] = {"16:9", "16:10", "21:9", "32:9"};
    static const int aw[] = {16, 16, 21, 32}, ah[] = {9, 10, 9, 9};
    int ai = 0;
    for (int i = 0; i < 4; i++)
        if (c->aspect_w == aw[i] && c->aspect_h == ah[i])
            ai = i;
    ImGui::BeginDisabled(!c->widescreen);
    hw_only_tag();
    if (choice("Aspect ratio", &ai, aspects, 4, "Match your display's shape.")) {
        c->aspect_w = aw[ai], c->aspect_h = ah[ai];
        mark(&c->aspect_w);
        mark(&c->aspect_h);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    static const char *const scales[] = {"1x (240p)", "2x (480p)", "3x (720p)", "4x (960p)",
                                         "5x (1200p)", "6x (1440p)", "7x (1680p)", "8x (1920p)"};
    int si = c->internal_scale - 1;
    restart_tag(c->internal_scale != L.info.running_scale);
    if (choice("Internal resolution", &si, scales, 8,
               ig ? "Resolution the 3D scenes are rendered at. Higher is sharper and needs more "
                    "GPU memory. Takes effect the next time the game starts."
                  : "Resolution the 3D scenes are rendered at. Higher is sharper and needs more GPU "
                    "memory; 4x suits most displays.")) {
        c->internal_scale = si + 1;
        mark(&c->internal_scale);
    }
    ImGui::BeginDisabled(hw_only);
    hw_only_tag();
    if (toggle("Precision geometry (PGXP)", &c->pgxp,
               "Removes the PlayStation's wobbling polygons and warping textures by keeping "
               "vertices at full precision and correcting texture perspective."))
        mark(&c->pgxp);
    ImGui::EndDisabled();
    if (toggle("Smooth scaling", &c->smooth,
               "Filter the image when scaling it to the window. Off gives sharp, square pixels."))
        mark(&c->smooth);
    restart_tag(c->vsync != L.info.running_vsync);
    if (toggle("VSync", &c->vsync,
               ig ? "Synchronise with the display to avoid tearing. Takes effect the next time "
                    "the game starts."
                  : "Synchronise with the display to avoid tearing."))
        mark(&c->vsync);
    if (toggle("Fullscreen", &c->fullscreen, "Also toggled in game with F11 or Cmd+Ctrl+F.")) {
        mark(&c->fullscreen);
        SDL_SetWindowFullscreen(L.win, c->fullscreen);
    }
    if (toggle("Show frame rate", &c->show_fps, "Show the game's frame rate in the window title."))
        mark(&c->show_fps);

    section("GAMEPLAY");
    if (toggle("Sprint", &c->sprint,
               "Hold Shift (or click the left stick on a controller) to move faster. "
               "Added by this port; not part of the original game."))
        mark(&c->sprint);
    ImGui::BeginDisabled(!c->sprint);
    float sp = (float)c->sprint_pct;
    if (slider("Sprint speed", &sp, 110, 250, 5, "%.0f%%",
               "Player movement speed while sprinting, relative to running.")) {
        c->sprint_pct = (int)sp;
        mark(&c->sprint_pct);
    }
    ImGui::EndDisabled();
    if (toggle("Infinite flashlight", &c->infinite_flashlight,
               "The flashlight's battery never runs down. In the original it lasts about 20 "
               "seconds and recharges while switched off. Added by this port."))
        mark(&c->infinite_flashlight);
    restart_tag(c->skip_intro != L.info.running_skip_intro);
    if (toggle("Skip intro movies", &c->skip_intro,
               ig ? "Start at the title menu, skipping the logos and the opening movie. Takes "
                    "effect the next time the game starts."
                  : "Start at the title menu, skipping the logos and the opening movie."))
        mark(&c->skip_intro);
    static const char *const cpus[] = {"Original (100%)", "150%", "200%", "300% (recommended)",
                                       "400%"};
    static const int cpv[] = {100, 150, 200, 300, 400};
    int ci = 3;
    for (int i = 0; i < 5; i++)
        if (c->cpu_pct == cpv[i])
            ci = i;
    if (choice("Frame rate boost", &ci, cpus, 5,
               "Emulated CPU speed. On original hardware busy scenes drop to 20-30 fps; 300% "
               "keeps a steady 60 fps. The game's timing is unaffected.")) {
        c->cpu_pct = cpv[ci];
        mark(&c->cpu_pct);
    }

    section("ENEMY DAMAGE");
    float ed = (float)c->enemy_damage_pct;
    if (slider("All enemies", &ed, 0, ENEMY_DAMAGE_MAX, 10, "%.0f%%",
               "Damage you take from enemy attacks, relative to the original game; the "
               "difficulty setting still applies on top. Falls, drowning and your own "
               "explosions are not affected. Added by this port.")) {
        c->enemy_damage_pct = (int)ed;
        mark(&c->enemy_damage_pct);
    }
    for (int g = 0; g < DMG_GROUP_COUNT; g++) {
        static char desc[DMG_GROUP_COUNT][200];
        snprintf(desc[g], sizeof desc[g], "%s Applied on top of \"All enemies\".",
                 g_enemy_groups[g].desc);
        float gv = (float)c->enemy_damage_group_pct[g];
        if (slider(g_enemy_groups[g].label, &gv, 0, ENEMY_DAMAGE_MAX, 10, "%.0f%%", desc[g])) {
            c->enemy_damage_group_pct[g] = (int)gv;
            mark(&c->enemy_damage_group_pct[g]);
        }
    }
    ImGui::Dummy(ImVec2(0, 8 * L.U));
    ImGui::EndChild();

    ImRect dr(ImVec2(cr.Min.x, cr.Max.y - desc_h), cr.Max);
    dl->AddLine(ImVec2(dr.Min.x, dr.Min.y), ImVec2(dr.Max.x, dr.Min.y), IM_COL32(104, 230, 120, 40));
    if (!L.desc)
        L.desc = ig ? "Left/right or click to change a setting. Changes apply right away and are "
                      "saved; settings marked \"applies on next start\" need a restart."
                    : "Left/right or click to change a setting. Changes are saved automatically "
                      "and apply when the game starts.";
    description(dl, dr);

    float by = L.S.y - 60 * L.U;
    if (footer_button("RESET TO DEFAULTS", 0, L.S.x * 0.5f - 150 * L.U, by)) {
        Settings d;
        settings_defaults(&d);
        snprintf(d.disc, sizeof d.disc, "%s", c->disc);
        memcpy(d.key, c->key, sizeof d.key);
        memcpy(d.pad, c->pad, sizeof d.pad);
        d.mouse_look = c->mouse_look, d.mouse_sens = c->mouse_sens;
        bool fs_changed = d.fullscreen != c->fullscreen;
        *c = d;
        *L.saved = d;
        L.dirty = true;
        if (fs_changed)
            SDL_SetWindowFullscreen(L.win, c->fullscreen);
    }
    if (footer_button("BACK", 1, L.S.x * 0.5f + 150 * L.U, by) || nav_back())
        open_page(PAGE_MAIN);
}

/* ---- controls page ---- */
SDL_GamepadType pad_type() {
    int n = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&n);
    SDL_GamepadType t = n > 0 ? SDL_GetGamepadTypeForID(pads[0]) : SDL_GAMEPAD_TYPE_UNKNOWN;
    SDL_free(pads);
    return t;
}

/* one binding cell; returns true when clicked (start capture) */
bool bind_cell(const char *id, const char *text, int row, int col, float x, float w, float y,
               float h, bool capturing, bool fixed, bool *clear) {
    int idx = nav_add(row, col, fixed);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushID(id);
    ImGui::BeginDisabled(fixed);
    bool clicked = ImGui::InvisibleButton("cell", ImVec2(w, h));
    ImGui::EndDisabled();
    nav_item_post(idx);
    bool hover = ImGui::IsItemHovered();
    bool hot = !fixed && ((hover && !L.nav_kb) || nav_is(idx));
    clicked |= nav_is(idx) && L.in_accept;
    if (!fixed && hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        *clear = true;
    if (!fixed && nav_is(idx) && L.cap.action < 0 &&
        (pressed(ImGuiKey_Delete, false) || pressed(ImGuiKey_GamepadFaceLeft, false)))
        *clear = true;
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a(x, y), b(x + w, y + h);
    ImU32 border = capturing ? C_ACCENT : hot ? IM_COL32(104, 230, 120, 190) : IM_COL32(70, 80, 75, 120);
    dl->AddRectFilled(a, b, capturing ? IM_COL32(104, 230, 120, 40) : hot ? IM_COL32(104, 230, 120, 22) : IM_COL32(12, 16, 14, 200), 4 * L.U);
    dl->AddRect(a, b, border, 4 * L.U, 0, (hot ? 1.5f : 1.0f) * L.U);
    const char *t = capturing ? "press..." : (text && *text ? text : "-");
    float fs = 17 * L.U;
    ImVec2 ts = text_size(L.f_body, fs, t);
    ImU32 tc = capturing ? C_ACCENT : fixed ? C_DIM : (text && *text) ? (hot ? C_HOT : C_TEXT) : C_FAINT;
    dl->PushClipRect(a, b, true);
    dl->AddText(L.f_body, fs, ImVec2(x + (w - ts.x) * 0.5f, y + (h - fs) * 0.5f), tc, t);
    dl->PopClipRect();
    return clicked && !fixed;
}

void draw_controls(ImDrawList *dl) {
    Settings *c = L.cfg;
    nav_begin();
    ImRect cr = panel(dl, "CONTROLS");
    float desc_h = 56 * L.U;
    ImGui::SetCursorScreenPos(cr.Min);
    ImGui::BeginChild("rows", ImVec2(cr.GetWidth(), cr.GetHeight() - desc_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    L.desc = nullptr;

    section("MOUSE");
    if (toggle("Mouse look", &c->mouse_look,
               "Mouse movement drives the right stick (look / turn). Click the game window to "
               "capture the mouse; F1 releases it."))
        mark(&c->mouse_look);
    ImGui::BeginDisabled(!c->mouse_look);
    if (slider("Mouse sensitivity", &c->mouse_sens, 1, 20, 0.5f, "%.1f", "How far the view turns per "
               "mouse movement."))
        mark(&c->mouse_sens);
    ImGui::EndDisabled();

    section("BUTTONS");
    float W = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    float colx[5] = {o.x + 16 * L.U, o.x + W * 0.30f, o.x + W * 0.46f, o.x + W * 0.62f,
                     o.x + W * 0.79f};
    float cw = W * 0.15f, cw_pad = W * 0.20f, rh = 30 * L.U, gap = 5 * L.U;
    float hs = 15 * L.U;
    dl = ImGui::GetWindowDrawList();
    const char *heads[5] = {"ACTION", "PLAYSTATION", "KEY / MOUSE", "ALTERNATE", "CONTROLLER"};
    for (int i = 0; i < 5; i++)
        dl->AddText(L.f_label, hs, ImVec2(colx[i] + (i >= 2 ? 4 * L.U : 0), o.y), C_DIM, heads[i]);
    float y = o.y + hs + 8 * L.U;
    SDL_GamepadType pt = pad_type();
    char nb[64];
    for (int a = 0; a < ACT_COUNT; a++) {
        const ActionInfo &ai = g_actions[a];
        bool row_hot = false;
        float fs = 19 * L.U;
        dl->AddText(L.f_label, fs, ImVec2(colx[0], y + (rh - fs) * 0.5f), C_TEXT, ai.label);
        dl->AddText(L.f_body, 16 * L.U, ImVec2(colx[1], y + (rh - 16 * L.U) * 0.5f), C_DIM,
                    *ai.pad ? ai.pad : "(port feature)");
        for (int col = 0; col < 3; col++) {
            char id[32];
            snprintf(id, sizeof id, "%d_%d", a, col);
            bool capturing = L.cap.action == a && L.cap.column == col;
            bool fixed = col == 2 && ai.stick;
            const char *text;
            if (col < 2)
                text = bind_key_name(c->key[a][col], nb, sizeof nb);
            else if (ai.stick)
                text = a < ACT_LOOK_UP ? "Left stick" : "Right stick";
            else
                text = bind_pad_name(c->pad[a], pt, nb, sizeof nb);
            bool clear = false;
            if (bind_cell(id, text, 1000 + a, col, colx[2 + col], col == 2 ? cw_pad : cw, y, rh,
                          capturing, fixed, &clear) &&
                L.cap.action < 0) {
                L.cap.action = a;
                L.cap.column = col;
                L.cap.started = SDL_GetTicks();
            }
            if (ImGui::IsItemHovered() || nav_is((int)L.nav_items.size() - 1))
                row_hot = true;
            if (clear) {
                if (col < 2) {
                    c->key[a][col] = 0;
                    mark(&c->key[a][col]);
                } else {
                    c->pad[a] = -1;
                    mark(&c->pad[a]);
                }
            }
        }
        if (row_hot)
            L.desc = "Click a box and press the key, mouse button or controller button to use. "
                     "Right-click or Delete clears it.";
        y += rh + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(W, 8 * L.U));
    ImGui::EndChild();

    ImRect dr(ImVec2(cr.Min.x, cr.Max.y - desc_h), cr.Max);
    dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(dr.Min.x, dr.Min.y), ImVec2(dr.Max.x, dr.Min.y), IM_COL32(104, 230, 120, 40));
    if (!L.desc)
        L.desc = "Controller sticks always drive the PlayStation analog sticks. Clicking a stick "
                 "bound to Sprint keeps sprinting until the left stick returns to centre.";
    description(dl, dr);

    float by = L.S.y - 60 * L.U;
    if (footer_button("RESET CONTROLS", 0, L.S.x * 0.5f - 150 * L.U, by)) {
        settings_default_bindings(c);
        c->mouse_look = true;
        c->mouse_sens = 6.0f;
        memcpy(L.saved->key, c->key, sizeof c->key);
        memcpy(L.saved->pad, c->pad, sizeof c->pad);
        L.saved->mouse_look = c->mouse_look;
        L.saved->mouse_sens = c->mouse_sens;
        L.dirty = true;
    }
    if ((footer_button("BACK", 1, L.S.x * 0.5f + 150 * L.U, by) || nav_back()) && L.cap.action < 0)
        open_page(PAGE_MAIN);
}

void draw_capture_overlay(ImDrawList *dl) {
    if (L.cap.action < 0)
        return;
    dl->AddRectFilled(ImVec2(0, 0), L.S, IM_COL32(0, 0, 0, 150));
    float w = 560 * L.U, h = 150 * L.U;
    ImVec2 p0((L.S.x - w) * 0.5f, (L.S.y - h) * 0.5f), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 12, 10, 245), 6 * L.U);
    dl->AddRect(p0, p1, C_ACCENT, 6 * L.U, 0, 1.5f * L.U);
    char t[128];
    snprintf(t, sizeof t, "%s", g_actions[L.cap.action].label);
    text_spaced(dl, L.f_title, 28 * L.U, L.S.x * 0.5f, p0.y + 22 * L.U, C_HOT, t, 2 * L.U);
    const char *m = L.cap.column == 2 ? "Press a controller button or trigger"
                                      : "Press a key or mouse button";
    float fs = 19 * L.U;
    float mw = text_size(L.f_body, fs, m).x;
    dl->AddText(L.f_body, fs, ImVec2(L.S.x * 0.5f - mw * 0.5f, p0.y + 70 * L.U), C_TEXT, m);
    const char *m2 = "Esc to cancel  -  Delete to clear";
    float mw2 = text_size(L.f_body, 16 * L.U, m2).x;
    dl->AddText(L.f_body, 16 * L.U, ImVec2(L.S.x * 0.5f - mw2 * 0.5f, p0.y + 104 * L.U), C_DIM, m2);
}

/* Binding capture sees raw SDL events before ImGui. Returns true if the
 * event was consumed. */
bool capture_event(const SDL_Event &e) {
    if (L.cap.action < 0)
        return false;
    Settings *c = L.cfg;
    int a = L.cap.action, col = L.cap.column;
    auto finish = [&]() { L.cap.action = -1; };
    auto bind_key = [&](int code) {
        for (int o = 0; o < ACT_COUNT; o++)       /* one action per key */
            for (int k = 0; k < KEY_SLOTS; k++)
                if (c->key[o][k] == code && !(o == a && k == col)) {
                    c->key[o][k] = 0;
                    mark(&c->key[o][k]);
                }
        c->key[a][col] = code;
        mark(&c->key[a][col]);
        finish();
    };
    auto bind_pad = [&](int code) {
        for (int o = 0; o < ACT_COUNT; o++)
            if (c->pad[o] == code && o != a) {
                c->pad[o] = -1;
                mark(&c->pad[o]);
            }
        c->pad[a] = code;
        mark(&c->pad[a]);
        finish();
    };
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat)
            return true;
        if (e.key.scancode == SDL_SCANCODE_F1 || e.key.scancode == SDL_SCANCODE_F2 ||
            e.key.scancode == SDL_SCANCODE_F11 || e.key.scancode == SDL_SCANCODE_F12)
            return true; /* fixed port keys: mouse release, overlay, fullscreen, screenshot */
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            finish();
        } else if (e.key.scancode == SDL_SCANCODE_DELETE) {
            if (col < 2) {
                c->key[a][col] = 0;
                mark(&c->key[a][col]);
            } else {
                c->pad[a] = -1;
                mark(&c->pad[a]);
            }
            finish();
        } else if (col < 2) {
            bind_key((int)e.key.scancode);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (col < 2 && e.button.button >= 1 && e.button.button <= 5)
            bind_key(BIND_MOUSE + e.button.button);
        else
            finish();
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (col == 2 && SDL_GetTicks() - L.cap.started > 150)
            bind_pad(e.gbutton.button);
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (col == 2 && e.gaxis.value > 20000) {
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                bind_pad(PAD_TRIGGER_L);
            else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                bind_pad(PAD_TRIGGER_R);
        }
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        /* releases still reach ImGui: the key that started the capture
         * (Enter) would otherwise stay held there and swallow its next press */
        return false;
    default:
        return false;
    }
}

const char *find_font(const char *const *paths) {
    for (; *paths; paths++)
        if (SDL_GetPathInfo(*paths, nullptr))
            return *paths;
    return nullptr;
}

void load_fonts() {
    ImGuiIO &io = ImGui::GetIO();
    static const char *const title[] = {"/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
                                        "/System/Library/Fonts/Supplemental/Futura.ttc", nullptr};
    static const char *const label[] = {"/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf",
                                        "/System/Library/Fonts/Avenir Next.ttc", nullptr};
    static const char *const body[] = {"/System/Library/Fonts/Helvetica.ttc",
                                       "/System/Library/Fonts/HelveticaNeue.ttc", nullptr};
    const char *p;
    ImFont *def = io.Fonts->AddFontDefault();
    L.f_title = (p = find_font(title)) ? io.Fonts->AddFontFromFileTTF(p, 20.0f) : def;
    L.f_label = (p = find_font(label)) ? io.Fonts->AddFontFromFileTTF(p, 20.0f) : def;
    L.f_body = (p = find_font(body)) ? io.Fonts->AddFontFromFileTTF(p, 20.0f) : def;
    if (!L.f_title) L.f_title = def;
    if (!L.f_label) L.f_label = def;
    if (!L.f_body) L.f_body = def;
}

void style() {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.ItemSpacing = ImVec2(0, 4);
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 4;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.41f, 0.90f, 0.47f, 0.35f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.90f, 0.47f, 0.6f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.41f, 0.90f, 0.47f, 0.8f);
    s.DisabledAlpha = 1.0f; /* rows draw their own disabled look */
}

/* ImGui context, style, fonts and backends for a window and GPU device */
void imgui_setup(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    /* keyboard/controller navigation is handled by the pages themselves */
    style();
    load_fonts();
    ImGui_ImplSDL3_InitForSDLGPU(win);
    ImGui_ImplSDLGPU3_InitInfo ii = {};
    ii.Device = dev;
    ii.ColorTargetFormat = fmt;
    ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&ii);
}

void frame() {
    ImGuiIO &io = ImGui::GetIO();
    L.S = io.DisplaySize;
    L.U = fmaxf(0.5f, fminf(L.S.y / 720.0f, L.S.x / 1100.0f));
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(L.S);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar;
    wf |= ImGuiWindowFlags_NoNav;
    ImGui::Begin("launcher", nullptr, wf);
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    if (L.ingame) {
        /* the paused game stays visible behind the menu */
        bg->AddRectFilled(ImVec2(0, 0), L.S, IM_COL32(0, 0, 0, L.page == PAGE_MAIN ? 165 : 185));
    } else {
        background(bg);
        if (L.page != PAGE_MAIN)
            bg->AddRectFilled(ImVec2(0, 0), L.S, IM_COL32(0, 0, 0, 120));
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    switch (L.page) {
    case PAGE_MAIN: L.ingame ? draw_pause(dl) : draw_main(dl); break;
    case PAGE_SETTINGS: draw_settings(dl); break;
    case PAGE_CONTROLS: draw_controls(dl); break;
    case PAGE_QUIT: draw_quit(dl); break;
    }
    draw_capture_overlay(ImGui::GetForegroundDrawList());
    ImGui::End();
}

/* Render the current draw data into an offscreen texture of the swapchain
 * format and write it as PNG (UI screenshots for testing/docs). */
void write_shot(ImDrawData *dd, SDL_GPUTextureFormat fmt) {
    int w = (int)(dd->DisplaySize.x * dd->FramebufferScale.x);
    int h = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = fmt;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(L.dev, &ti);
    SDL_GPUTransferBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    bi.size = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(L.dev, &bi);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(L.dev);
    ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = t;
    ct.clear_color = SDL_FColor{0.024f, 0.031f, 0.031f, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
    SDL_EndGPURenderPass(rp);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg = {};
    reg.texture = t;
    reg.w = (Uint32)w;
    reg.h = (Uint32)h;
    reg.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(cp, &reg, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(L.dev, true, &f, 1);
    SDL_ReleaseGPUFence(L.dev, f);
    const uint8_t *px = (const uint8_t *)SDL_MapGPUTransferBuffer(L.dev, tb, false);
    bool bgra = fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3 + 0] = px[i * 4 + (bgra ? 2 : 0)];
        rgb[i * 3 + 1] = px[i * 4 + 1];
        rgb[i * 3 + 2] = px[i * 4 + (bgra ? 0 : 2)];
    }
    SDL_UnmapGPUTransferBuffer(L.dev, tb);
    png_write_rgb(L.shot_path, rgb, w, h);
    free(rgb);
    SDL_ReleaseGPUTransferBuffer(L.dev, tb);
    SDL_ReleaseGPUTexture(L.dev, t);
}

/* Feed one scripted key press every few frames (down, then up). */
void run_script() {
    if (L.script_pos >= L.script.size())
        return;
    int t = L.script_tick++ % 4;
    if (t != 0 && t != 1)
        return;
    SDL_Event e = {};
    e.type = t == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(L.win);
    e.key.scancode = L.script[L.script_pos];
    e.key.key = SDL_GetKeyFromScancode(e.key.scancode, SDL_KMOD_NONE, false);
    e.key.down = t == 0;
    SDL_PushEvent(&e);
    if (t == 1)
        L.script_pos++;
}

/* F11 / Cmd+Ctrl+F: toggle fullscreen (saved like the Settings toggle) */
bool fullscreen_key(const SDL_Event &e) {
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat ||
        !(e.key.scancode == SDL_SCANCODE_F11 ||
          (e.key.scancode == SDL_SCANCODE_F && (e.key.mod & SDL_KMOD_GUI) &&
           (e.key.mod & SDL_KMOD_CTRL))))
        return false;
    L.cfg->fullscreen = !L.cfg->fullscreen;
    mark(&L.cfg->fullscreen);
    SDL_SetWindowFullscreen(L.win, L.cfg->fullscreen);
    return true;
}

} // namespace

extern "C" void launcher_debug_keys(const char *list) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        SDL_Scancode sc = SDL_GetScancodeFromName(tok);
        if (sc != SDL_SCANCODE_UNKNOWN)
            L.script.push_back(sc);
        else
            SDL_Log("launcher: unknown key '%s' in script", tok);
    }
}

extern "C" void launcher_debug_shot(const char *path, const char *page) {
    if (page && !strcmp(page, "start")) {
        L.auto_start = true;
        return;
    }
    L.shot_path = path;
    L.page = !page ? PAGE_MAIN : !strcmp(page, "settings") ? PAGE_SETTINGS
             : !strcmp(page, "controls")                   ? PAGE_CONTROLS
                                                           : PAGE_MAIN;
}

extern "C" bool launcher_run(SDL_Window *win, Settings *cfg, Settings *saved, const char *path) {
    L.win = win;
    L.cfg = cfg;
    L.saved = saved;
    L.path = path;
    L.result = -1;
    if (!L.shot_path)
        L.page = PAGE_MAIN;
    L.menu_sel = 0;

    L.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB |
                                    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL,
                                false, nullptr);
    if (!L.dev || !SDL_ClaimWindowForGPUDevice(L.dev, win)) {
        SDL_Log("launcher: GPU device unavailable: %s", SDL_GetError());
        if (L.dev)
            SDL_DestroyGPUDevice(L.dev);
        L.dev = nullptr;
        return cfg->disc[0] != 0; /* fall back to starting directly */
    }
    SDL_SetGPUSwapchainParameters(L.dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);
    imgui_setup(win, L.dev, SDL_GetGPUSwapchainTextureFormat(L.dev, win));

    if (cfg->fullscreen)
        SDL_SetWindowFullscreen(win, true);
    check_disc();
    if (L.disc != DISC_OK)
        L.menu_sel = 1; /* "select disc image" */
    save_if_dirty();

    while (L.result < 0) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)))
                L.result = 0;
            if (capture_event(e))
                continue;
            if (fullscreen_key(e))
                continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
        }
        if (L.cap.action >= 0 && SDL_GetTicks() - L.cap.started > 8000)
            L.cap.action = -1; /* stale capture (e.g. no controller) */
        poll_picked();
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        ImDrawData *dd = ImGui::GetDrawData();
        run_script();
        bool script_done = L.script_pos >= L.script.size();
        if ((L.shot_path || L.auto_start) && script_done && ++L.shot_frames == 6) {
            if (L.shot_path)
                write_shot(dd, SDL_GetGPUSwapchainTextureFormat(L.dev, win));
            L.result = L.auto_start && L.disc == DISC_OK ? 1 : 0;
        }
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(L.dev);
        SDL_GPUTexture *sw = nullptr;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &sw, nullptr, nullptr);
        if (sw && dd->DisplaySize.x > 0 && dd->DisplaySize.y > 0) {
            ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
            SDL_GPUColorTargetInfo ct = {};
            ct.texture = sw;
            ct.clear_color = SDL_FColor{0.024f, 0.031f, 0.031f, 1.0f};
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
            SDL_EndGPURenderPass(rp);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    save_if_dirty();

    SDL_WaitForGPUIdle(L.dev);
    if (L.art)
        SDL_ReleaseGPUTexture(L.dev, L.art);
    L.art = nullptr;
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(L.dev, win);
    SDL_DestroyGPUDevice(L.dev);
    L.dev = nullptr;
    return L.result == 1;
}

/* ---- in-game overlay ---- */

extern "C" bool overlay_open(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt,
                             Settings *cfg, Settings *saved, const char *path,
                             const OverlayInfo *info) {
    if (!win || !dev || fmt == SDL_GPU_TEXTUREFORMAT_INVALID)
        return false;
    L.win = win;
    L.dev = dev;
    L.cfg = cfg;
    L.saved = saved;
    L.path = path;
    L.info = *info;
    if (!L.imgui_ready) {
        /* created on first use and kept for the rest of the run */
        imgui_setup(win, dev, fmt);
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
        L.imgui_ready = true;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    L.ingame = true;
    L.page = PAGE_MAIN;
    L.menu_sel = 0;
    L.quit_sel = 0;
    L.nav_items.clear();
    L.nav_prev.clear();
    L.nav_kb = false;
    L.cap.action = -1;
    L.tag = nullptr;
    L.pad_blocked = true;
    L.overlay_result = OVERLAY_OPEN;
    return true;
}

extern "C" void overlay_event(const SDL_Event *ev) {
    if (!L.ingame)
        return;
    const SDL_Event &e = *ev;
    if (capture_event(e))
        return;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F2) {
        L.overlay_result = OVERLAY_RESUME;
        return;
    }
    if (fullscreen_key(e))
        return;
    ImGui_ImplSDL3_ProcessEvent(&e);
}

extern "C" int overlay_update(void) {
    if (!L.ingame)
        return OVERLAY_RESUME;
    if (L.cap.action >= 0 && SDL_GetTicks() - L.cap.started > 8000)
        L.cap.action = -1; /* stale capture (e.g. no controller) */
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (L.pad_blocked) {
        bool held = false;
        for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; k++)
            held |= ImGui::IsKeyDown((ImGuiKey)k);
        L.pad_blocked = held;
    }
    if (L.cap.action < 0 && pressed(ImGuiKey_GamepadStart, false))
        L.overlay_result = OVERLAY_RESUME;
    frame();
    ImGui::Render();
    if (L.overlay_result != OVERLAY_OPEN)
        save_if_dirty();
    return L.overlay_result;
}

extern "C" void overlay_render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int w, int h) {
    (void)w, (void)h;
    if (!L.ingame || !L.imgui_ready)
        return;
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0)
        return;
    ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_LOAD; /* on top of the game image */
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
    SDL_EndGPURenderPass(rp);
}

extern "C" void overlay_close(void) {
    if (!L.ingame)
        return;
    save_if_dirty();
    L.cap.action = -1;
    L.ingame = false;
}
