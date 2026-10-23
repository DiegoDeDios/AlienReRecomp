/*
 * Native replacements for recompiled functions (listed in
 * tools/overrides_*.txt, which the recompiler leaves out).
 */
#include <math.h>
#include <stdlib.h>

#include "camera.h"
#include "enemy_damage.h"
#include "pgxp.h"
#include "psx.h"
#include "savestate.h"
#include "widescreen.h"

/* libetc VSync's wait helper: v_wait(target, n) spins until the vblank
 * counter reaches `target`, giving up after n<<15 loop iterations. That
 * iteration-based timeout fires early once the emulated CPU runs faster
 * than the original, so the native version instead advances emulated time
 * straight to the next scheduled event until the vblank interrupt has
 * bumped the counter. Besides being correct at any CPU speed, it avoids
 * burning host time in the busy-wait. */
static void vsync_wait(CPU *c, uint32_t vcount_addr) {
    int32_t target = (int32_t)c->r[4];
    int64_t limit = c->cycles + 30LL * PSX_NTSC_FRAME_CYCLES;
    while ((int32_t)mem_r32(vcount_addr) < target) {
        if (c->cycles >= limit) {
            LOGW("vsync", "wait for %d timed out (counter %d)", target,
                 (int32_t)mem_r32(vcount_addr));
            break;
        }
        if (c->next_event > c->cycles)
            c->cycles = c->next_event;
        cpu_service_events(c);
    }
}

void ALIEN_80082C4C(CPU *c) { vsync_wait(c, 0x8009D240u); }
void SLUS_800163D4(CPU *c) { vsync_wait(c, 0x8001F6C8u); }

/* libetc VSync(mode): the menus wait in it and gameplay polls it with -1
 * every frame, so its entry is where save states are taken and restored
 * (savestate.c) */
void ALIEN_80082AD4__orig(CPU *c);
void ALIEN_80082AD4(CPU *c) {
    state_safe_point(c);
    ALIEN_80082AD4__orig(c);
}

/* Screen-space quad subdivision (hand-written renderer helper). For large
 * near-camera polygons the game splits a textured quad (POLY_GT4 at a0)
 * into up to 4x4 smaller quads by averaging corner positions/UVs in screen
 * space (a2 = levels), which hides affine texture warping on the PS1. With
 * PGXP the original quad is rendered with perspective-correct texturing,
 * which is exact, so when all four corners are tracked projections we ask
 * for level 0 (the routine then emits the quad unchanged). That includes
 * corners at or behind the camera: the averaged midpoints of their clamped
 * positions would be garbage, while the renderer clips the whole quad. */
void ALIEN_800A81BC__orig(CPU *c);
void ALIEN_800A81BC(CPU *c) {
    if (g_pgxp && (int32_t)c->r[6] > 0) {
        static const uint32_t corner[4] = {8, 20, 32, 44};
        uint32_t pkt = c->r[4];
        bool tracked = true;
        for (int i = 0; i < 4 && tracked; i++) {
            PgxpVtx v;
            tracked = pgxp_lookup(pkt + corner[i], mem_r32(pkt + corner[i]), &v);
        }
        if (tracked)
            c->r[6] = 0;
    }
    ALIEN_800A81BC__orig(c);
}

/* Player movement speed (0x8005E8E8, called from the player update
 * 0x8005ECBC): move_delta(obj, dir, target, accel, out). `target` is the
 * stick deflection times the stance's top speed (7.2 units/frame standing,
 * 2.8 crouched, 3.5 swimming; 20.12 fixed point) and `accel` the ramp rate:
 * obj+0x88 approaches `target` by accel/8 per vblank, and the per-frame
 * displacement written to `out` follows from it. Sprint scales both, so
 * the ramp takes as long as a normal start and releasing Shift eases back
 * down to walking speed. Only the player, only standing (stance byte
 * obj+0x171 = 0), never during attract-mode demo playback. */
#define PLAYER_OBJ_PTR 0x800A49E8u /* gp+0x474 */
#define DEMO_PLAYBACK  0x800A48C4u

bool g_sprint;
int g_sprint_pct = 190;

void ALIEN_8005E8E8__orig(CPU *c);
void ALIEN_8005E8E8(CPU *c) {
    uint32_t obj = c->r[4];
    if (g_sprint && g_sprint_pct != 100 && obj && obj == mem_r32(PLAYER_OBJ_PTR) &&
        mem_r8(obj + 0x171) == 0 && mem_r32(DEMO_PLAYBACK) == 0) {
        c->r[6] = (uint32_t)((int64_t)(int32_t)c->r[6] * g_sprint_pct / 100);
        c->r[7] = (uint32_t)((int64_t)(int32_t)c->r[7] * g_sprint_pct / 100);
    }
    ALIEN_8005E8E8__orig(c);
}

/* Flashlight update (0x800625CC, every frame for the player; a1 = the
 * player's state block, also at gp+0x478). While the light is on (state
 * +0x40 holds the light object) the battery, kept as the flashlight item's
 * amount in the inventory (+0x44), drains one unit per vblank and the light
 * goes out when it reaches 0; while off it recharges two units per vblank
 * up to the capacity, and an empty flashlight cannot be switched on.
 * Infinite flashlight keeps the battery full around the update, so it never
 * runs down (the HUD shows 100%). Not during attract-mode demo playback. */
#define INVENTORY_PTR       0x800A49F0u /* gp+0x47C */
#define FLASHLIGHT_CAPACITY 0x800A7B10u /* 1200 vblanks of light */

bool g_infinite_flashlight = true;

static void flashlight_fill(void) {
    uint32_t inv = mem_r32(INVENTORY_PTR);
    if (inv)
        mem_w16(inv + 0x44, (uint16_t)mem_r32(FLASHLIGHT_CAPACITY));
}

void ALIEN_800625CC__orig(CPU *c);
void ALIEN_800625CC(CPU *c) {
    bool keep = g_infinite_flashlight && mem_r32(DEMO_PLAYBACK) == 0;
    if (keep)
        flashlight_fill();
    ALIEN_800625CC__orig(c);
    if (keep)
        flashlight_fill();
}

/* Enemy damage. Everything that hurts an object goes through
 * damage(obj, amount, kind) at 0x800519D0, which for the player scales the
 * amount by the difficulty (options +4: Easy 1.0, Normal 1.2, Hard 1.5,
 * Deadly 2.0, from the table at 0x800A7A44) and subtracts it from health
 * (obj+0x148, 4096 = full). Enemy attacks reach the player as messages,
 * delivered by 0x8004DEA8(target, msg): code 4 (hit, e.g. a melee strike:
 * 0x8006FB9C sends 4096 / hits-to-kill), 7, and 402 (explosion). Their
 * damage is msg+0x14 and the sender's object type msg+0x0A (the message
 * constructor 0x8004D9B0 fills both). Acid blood (kind 2: message 8 from an
 * alien that is hit, and acid particles, 0x800152C4) calls damage()
 * directly. Falls, drowning and other hazards are left alone, as are
 * explosions the player causes. */
#define MSG_TYPE   0x0A  /* sender's object type (obj+0x142) */
#define MSG_DAMAGE 0x14
#define MSG_CODE   0x24
#define MSG_SENDER 0x2C

int g_enemy_damage_pct = 100;
int g_enemy_damage_group_pct[DMG_GROUP_COUNT] = {100, 100, 100, 100, 100, 100, 100, 100};

/* Object types (obj+0x142; names in the table at 0x8009905C). Returns the
 * group, DMG_GROUP_COUNT for other creatures (eggs, clones, egg sac: only
 * the "all enemies" value applies), or -1 for anything that is not an
 * enemy (playable characters, level objects). */
static int enemy_group(uint32_t type) {
    switch (type) {
    case 19: return DMG_FACEHUGGERS;
    case 20: return DMG_CHESTBURSTERS;
    case 21: case 22: return DMG_ALIENS;
    case 23: return DMG_NEWBORN;
    case 24: case 25: return DMG_QUEEN;
    case 33: return DMG_SENTRY_TANK;
    case 18: case 34: return DMG_GROUP_COUNT;
    default:
        if ((type >= 9 && type <= 17) || (type >= 35 && type <= 39))
            return DMG_MARINES;
        if (type >= 26 && type <= 32)
            return DMG_GROUP_COUNT;
        return -1;
    }
}

static uint32_t enemy_damage(uint32_t amount, int group) {
    int64_t pct = (int64_t)g_enemy_damage_pct *
                  (group < DMG_GROUP_COUNT ? g_enemy_damage_group_pct[group] : 100);
    int32_t a = (int32_t)amount;
    if (pct == 10000 || a <= 0)
        return amount;
    int64_t v = ((int64_t)a * pct + 5000) / 10000;
    if (v < 1 && pct > 0)
        v = 1; /* a hit still registers */
    return (uint32_t)(v > 0x7FFF ? 0x7FFF : v);
}

static bool hurts_player(uint32_t obj) {
    return obj && obj == mem_r32(PLAYER_OBJ_PTR) && mem_r32(DEMO_PLAYBACK) == 0;
}

void ALIEN_8004DEA8__orig(CPU *c);
void ALIEN_8004DEA8(CPU *c) {
    uint32_t msg = c->r[5];
    if (msg && hurts_player(c->r[4])) {
        uint32_t code = mem_r16(msg + MSG_CODE);
        if ((code == 4 || code == 7 || code == 402) && mem_r32(msg + MSG_SENDER) != c->r[4]) {
            uint32_t type = mem_r16(msg + MSG_TYPE);
            int group = enemy_group(type);
            if (group >= 0) {
                uint32_t a = mem_r32(msg + MSG_DAMAGE), b = enemy_damage(a, group);
                if (b != a) {
                    mem_w32(msg + MSG_DAMAGE, b);
                    LOGD("damage", "message %u from type %u: %d -> %d", code, type, (int)a,
                         (int)b);
                }
            }
        }
    }
    ALIEN_8004DEA8__orig(c);
}

void ALIEN_800519D0__orig(CPU *c);
void ALIEN_800519D0(CPU *c) {
    if (c->r[6] == 2 && hurts_player(c->r[4])) {
        uint32_t a = c->r[5];
        c->r[5] = enemy_damage(a, DMG_ACID);
        if (c->r[5] != a)
            LOGD("damage", "acid: %d -> %d", (int)a, (int)c->r[5]);
    }
    ALIEN_800519D0__orig(c);
}

/* Camera. Each frame the main loop processes the pads (0x80031594) and
 * builds the gameplay command block at 0x800BDBE0 (0x80032530): +0x00 turn
 * and +0x04 look, -4096..4096 (full right / full up = -4096). The player
 * update (0x8005ECBC) then turns by turn * rate >> 12, rate = TURN SPEED
 * (options +0x11) * 27 / 8 + 13, or * 5 / 2 + 10 while swimming (stance
 * 3), times the frame step; and looks up/down (camera pitch, clamped to
 * +-600) by look * rate >> 12, rate = LOOK SPEED (+0x12) * 11 / 4 + 16 --
 * when +0x18 is set (dual-stick layouts); otherwise the stick sets a target
 * pitch. Controller input reaches the command block two frames late (libpad
 * buffering, then the next frame's processing), averaged over samples, and
 * only past a dead zone of 38% of the stick's travel at the default STICK
 * DEAD ZONE (+0x10). The wrapper puts in the right stick as read at the last
 * vblank, with its own dead zone, curve and speed, and adds mouse look
 * converted with the inverse of the rate, so the camera turns by exactly
 * the mouse movement. */
#define CMD_TURN       0x800BDBE0u
#define CMD_LOOK       0x800BDBE4u
#define CMD_LOOK_RATE  0x800BDBF8u
#define CMD_VARIANT_A  0x800BDC20u /* select the control type's alternate */
#define CMD_VARIANT_B  0x800BDC28u /* axis mappings */
#define PAD0_AXES      0x800BDAF0u /* processed: left X, left Y, right X, right Y */
#define CONTROL_CONFIG 0x800A4704u /* gp+0x190; +0xC8: command slot of each axis */
#define OPTIONS        0x80096BBCu /* +0x0E invert Y, +0x11/+0x12 turn/look speed */
#define FRAME_STEP     0x800A86A4u /* gp+0x4130: vblanks since the last frame */

float g_cam_mouse_x, g_cam_mouse_y;
int g_cam_mouse_age;
bool g_stick_direct = true;
int g_stick_dead_zone = 10;
float g_cam_stick_curve = 1.5f;
int g_cam_stick_speed = 150;

/* Raw stick bytes (0x80 = centre) to -1..1 with a radial dead zone and a
 * response curve. At or past the edge the values are kept as they are, so
 * full diagonals (keyboard) are full on both axes, as in the game. */
static void stick_shape(uint8_t rawx, uint8_t rawy, float curve, float *x, float *y) {
    float fx = (float)(rawx - 128) / 127.0f, fy = (float)(rawy - 128) / 127.0f;
    fx = fx < -1 ? -1 : fx > 1 ? 1 : fx;
    fy = fy < -1 ? -1 : fy > 1 ? 1 : fy;
    float m = sqrtf(fx * fx + fy * fy), dz = (float)g_stick_dead_zone / 100.0f, s = 0;
    if (m >= 1)
        s = 1;
    else if (m > dz)
        s = powf((m - dz) / (1 - dz), curve) / m;
    *x = fx * s, *y = fy * s;
}

/* command slot of analog axis `axis` in the current control type */
static int axis_slot(int axis) {
    uint32_t cfg = mem_r32(CONTROL_CONFIG);
    if (!cfg)
        return -1;
    int v = mem_r32(CMD_VARIANT_A) ? 1 : mem_r32(CMD_VARIANT_B) ? 2 : 0;
    return (int32_t)mem_r32(cfg + 0xC8 + (uint32_t)(v * 16 + axis * 4));
}

/* The fresh stick value replaces what the game made of the same axis (it
 * equals the processed, older value); anything else mapped to the slot,
 * such as the d-pad, wins when larger, as in the game's own builder. */
static void stick_to_slot(uint32_t slot, int axis, int32_t v) {
    int32_t game = (int32_t)mem_r32(slot);
    int32_t old = (int32_t)mem_r32(PAD0_AXES + (uint32_t)axis * 4);
    if (game != old && abs(game) > abs(v))
        return;
    mem_w32(slot, (uint32_t)v);
}

static void camera_stick(void) {
    uint8_t rx, ry, lx, ly;
    float x, y;
    pad_get_sticks(0, &rx, &ry, &lx, &ly);
    stick_shape(rx, ry, g_cam_stick_curve, &x, &y);
    float k = 4096.0f * (float)g_cam_stick_speed / 100.0f;
    int32_t turn = (int32_t)lroundf(-x * k), look = (int32_t)lroundf(y * k);
    if (mem_r8(OPTIONS + 0x0E))
        look = -look;
    if (axis_slot(2) == 0)
        stick_to_slot(CMD_TURN, 2, turn);
    if (axis_slot(3) == 1)
        stick_to_slot(CMD_LOOK, 3, look);
}

/* Movement stick: the left stick's processed values (pad state +0x00
 * left X, +0x04 left Y; right/up = -4096/+4096) are replaced with the stick
 * as it is now, linear past the dead zone, right before the command block
 * is built from them (0x80032530), which maps them through the control
 * type and derives the move direction (+0x10, ratan2) and magnitude
 * (+0x14). */
static void movement_stick(void) {
    uint8_t rx, ry, lx, ly;
    float x, y;
    pad_get_sticks(0, &rx, &ry, &lx, &ly);
    stick_shape(lx, ly, 1.0f, &x, &y);
    mem_w32(PAD0_AXES + 0, (uint32_t)(int32_t)lroundf(-x * 4096.0f));
    mem_w32(PAD0_AXES + 4, (uint32_t)(int32_t)lroundf(-y * 4096.0f));
}

static bool player_input_live(void) {
    return mem_r32(PLAYER_OBJ_PTR) && mem_r32(DEMO_PLAYBACK) == 0;
}

void ALIEN_80032530__orig(CPU *c);
void ALIEN_80032530(CPU *c) {
    if (g_stick_direct && player_input_live())
        movement_stick();
    ALIEN_80032530__orig(c);
}

/* command value that changes the game's (cmd * rate) >> 12 by exactly
 * `delta` (the smallest one that rounds down to the target) */
static int32_t cmd_for_delta(int32_t cmd, int rate, int32_t delta) {
    int64_t p = (int64_t)cmd * rate;
    int64_t t = (p >= 0 ? p / 4096 : -((-p + 4095) / 4096)) + delta; /* floor + delta */
    int64_t a = t * 4096;
    return (int32_t)(a >= 0 ? (a + rate - 1) / rate : a / rate); /* ceil(a / rate) */
}

static void camera_mouse(uint32_t player) {
    float mx = g_cam_mouse_x, my = g_cam_mouse_y;
    g_cam_mouse_age = 0;
    if (mx == 0 && my == 0)
        return;
    int fs = (int32_t)mem_r32(FRAME_STEP);
    fs = fs < 1 ? 1 : fs;
    int ts = (int)mem_r8(OPTIONS + 0x11), ls = (int)mem_r8(OPTIONS + 0x12);
    bool swim = (int8_t)mem_r8(player + 0x171) == 3;
    int turn_rate = (swim ? ((ts * 5) >> 1) + 10 : ((ts * 27) >> 3) + 13) * fs;
    int look_rate = (swim ? ((ts * 5) >> 1) + 10 : ((ls * 11) >> 2) + 16) * fs;

    /* whole units now, the rest next frame; turning right lowers the yaw */
    int32_t d = (int32_t)lroundf(mx);
    mem_w32(CMD_TURN, (uint32_t)cmd_for_delta((int32_t)mem_r32(CMD_TURN), turn_rate, -d));
    g_cam_mouse_x = mx - (float)d;

    if (swim || mem_r32(CMD_LOOK_RATE)) {
        float inv = mem_r8(OPTIONS + 0x0E) ? -1.0f : 1.0f; /* the game's Invert Y */
        int32_t l = (int32_t)lroundf(my * inv);
        mem_w32(CMD_LOOK, (uint32_t)cmd_for_delta((int32_t)mem_r32(CMD_LOOK), look_rate, l));
        g_cam_mouse_y = my - (float)l * inv;
    } else {
        g_cam_mouse_y = 0; /* the stick sets a target pitch here: no mouse look */
    }
}

void ALIEN_8005ECBC__orig(CPU *c);
void ALIEN_8005ECBC(CPU *c) {
    uint32_t obj = c->r[4];
    if (obj && obj == mem_r32(PLAYER_OBJ_PTR) && mem_r32(DEMO_PLAYBACK) == 0) {
        if (g_stick_direct)
            camera_stick();
        camera_mouse(obj);
    }
    ALIEN_8005ECBC__orig(c);
}

/* main (0x80020350). Option defaults live in the options block at
 * 0x80096BBC (.data); at boot the game replaces them with the options
 * stored in the newest save on the memory card, if there is one. The
 * original ships with "Invert Y" (byte +0x0E) on; default it to off. A
 * player who prefers inverted look can still turn it on in Control Options
 * and it is restored from their saves. */
#define OPT_INVERT_Y 0x80096BCAu

void ALIEN_80020350__orig(CPU *c);
void ALIEN_80020350(CPU *c) {
    mem_w8(OPT_INVERT_Y, 0);
    ALIEN_80020350__orig(c);
}

/* Field of view. In game the frame's camera setup (0x8001EEC0) sets the
 * GTE projection distance H to 384 + the s16 at 0x800A476E, an offset the
 * game itself always leaves at 0 (reset at level start by 0x8005BF1C). The
 * 3D screen is 384 x 240 pixels (OFX 192, OFY 120) and the camera matrix
 * scales the vertical axis by 0.8, so the horizontal field of view is
 * 2 atan(192 / (H k)) -- 53 degrees at 4:3, 67 with the 16:9 widescreen
 * scale k -- and the vertical one 2 atan(120 / (0.8 H)), 43 degrees. A wider
 * view is a smaller H: written into that offset before the setup runs, so
 * everything the game projects in the frame (and PGXP) uses it.
 *
 * The first-person weapon (0x80020848, drawn through 0x80020544) is kept at
 * the original H: the models are built for that framing, and a wider one
 * shows parts that were always off screen. */
#define CAMERA_H_OFFSET 0x800A476Eu
#define CAMERA_H        384.0
#define SCREEN_HALF_W   192.0
#define FOV_PI          3.14159265358979323846

int g_fov_deg;

double fov_original_deg(double ws_k) {
    return 2.0 * atan(SCREEN_HALF_W / (CAMERA_H * ws_k)) * 180.0 / FOV_PI;
}

static int fov_h_offset(void) {
    double k = g_ws_k > 0 ? g_ws_k : 1.0;
    int deg = g_fov_deg > FOV_MAX_DEG ? FOV_MAX_DEG : g_fov_deg;
    if (deg <= fov_original_deg(k))
        return 0;
    double h = SCREEN_HALF_W / (k * tan(deg * FOV_PI / 360.0));
    return (int)lround(h - CAMERA_H);
}

void ALIEN_8001EEC0__orig(CPU *c);
void ALIEN_8001EEC0(CPU *c) {
    mem_w16(CAMERA_H_OFFSET, (uint16_t)(int16_t)fov_h_offset());
    ALIEN_8001EEC0__orig(c);
}

void ALIEN_80020848__orig(CPU *c);
void ALIEN_80020848(CPU *c) {
    /* the offset this frame's H was set up with */
    int32_t off = (int16_t)mem_r16(CAMERA_H_OFFSET);
    uint32_t h = gte_read_ctrl(c, 26);
    if (off)
        gte_write_ctrl(c, 26, (uint32_t)((int32_t)h - off));
    ALIEN_80020848__orig(c);
    if (off)
        gte_write_ctrl(c, 26, h);
}
