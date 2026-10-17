/*
 * Native replacements for recompiled functions (listed in
 * tools/overrides_*.txt, which the recompiler leaves out).
 */
#include "enemy_damage.h"
#include "pgxp.h"
#include "psx.h"

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

/* Screen-space quad subdivision (hand-written renderer helper). For large
 * near-camera polygons the game splits a textured quad (POLY_GT4 at a0)
 * into up to 4x4 smaller quads by averaging corner positions/UVs in screen
 * space (a2 = levels), which hides affine texture warping on the PS1. With
 * PGXP the original quad is rendered with perspective-correct texturing,
 * which is exact, so when all four corners carry precise data we ask for
 * level 0 (the routine then emits the quad unchanged). */
void ALIEN_800A81BC__orig(CPU *c);
void ALIEN_800A81BC(CPU *c) {
    if (g_pgxp && (int32_t)c->r[6] > 0) {
        static const uint32_t corner[4] = {8, 20, 32, 44};
        uint32_t pkt = c->r[4];
        bool precise = true;
        for (int i = 0; i < 4 && precise; i++) {
            PgxpVtx v;
            precise = pgxp_lookup(pkt + corner[i], mem_r32(pkt + corner[i]), &v);
        }
        if (precise)
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
