/*
 * USA / Europe releases: detection and address translation (region.h).
 *
 * The European ALIEN.BIN (SLES-02913) is the USA program rebuilt with a
 * little more code and data (PAL video mode support, longer text), so the
 * same functions and variables sit at other addresses. Every USA address the
 * port uses was located in the European program by its code: the PsyQ
 * library by signature, game functions by their call sites and the variables
 * they use, variables by the instructions that read them. In short:
 *   text 0x8001xxxx-0x80020xxx     +0x04C
 *   text 0x80030xxx-0x80034xxx     +0x188
 *   text 0x8004Dxxx                +0x150
 *   text 0x80050xxx-0x80081xxx     +0x16C (libraries too)
 *   .data options block            +0x210, libetc data +0x1B0
 *   .sdata (gp 0x800A4574 -> 0x800A4728), hand-written renderer code
 *   after it and .bss              +0x1B4
 * The boot program (SLES_029.13 instead of SLUS_006.33) is a different
 * build; only its VSync wait helper is used.
 */
#include "region.h"

#include <string.h>

#include "psx.h"

GameRegion g_region = REGION_USA;

typedef struct {
    uint32_t usa, eu;
} AddrPair;

/* sorted by USA address */
static const AddrPair k_eu[] = {
    /* boot program SLUS_006.33 -> SLES_029.13 */
    {0x800163D4u, 0x800175E0u}, /* boot: libetc v_wait */
    /* ALIEN.BIN code */
    {0x8001EEC0u, 0x8001EF0Cu}, /* per-frame camera setup */
    {0x8001F6C8u, 0x800216ACu}, /* boot: libetc vblank counter */
    {0x8001FC40u, 0x8001FC8Cu}, /* return address of the section-load VSync(3) */
    {0x8001FC6Cu, 0x8001FCB8u}, /* return address of the end-of-frame wait */
    {0x80020350u, 0x8002039Cu}, /* main */
    {0x80020848u, 0x80020894u}, /* first-person weapon pass */
    {0x80030E68u, 0x80030FF0u}, /* end of frame: wait for the refresh */
    {0x80032530u, 0x800326B8u}, /* build the gameplay command block */
    {0x80033A9Cu, 0x80033C24u}, /* HUD panels */
    {0x800343FCu, 0x80034584u}, /* motion tracker */
    {0x80034A50u, 0x80034BD8u}, /* crosshair */
    {0x8004DEA8u, 0x8004DFF8u}, /* deliver a message to an object */
    {0x80050730u, 0x8005089Cu}, /* move an object */
    {0x800519D0u, 0x80051B3Cu}, /* apply damage */
    {0x800524F0u, 0x8005265Cu}, /* add a pickup to the inventory */
    {0x8005E8E8u, 0x8005EA54u}, /* player movement speed helper */
    {0x8005ECBCu, 0x8005EE28u}, /* player update */
    {0x800625CCu, 0x80062738u}, /* flashlight update */
    {0x800698A8u, 0x80069A14u}, /* return address of the enemy-leap move */
    {0x80080FE4u, 0x80081150u}, /* lights on level objects */
    {0x80082AD4u, 0x80082C40u}, /* libetc VSync */
    {0x80082C4Cu, 0x80082DB8u}, /* libetc v_wait */
    /* ALIEN.BIN data */
    {0x80096BBCu, 0x80096DCCu}, /* options block */
    {0x80096BCAu, 0x80096DDAu}, /* options +0x0E: invert Y */
    {0x8009D240u, 0x8009D3F0u}, /* libetc vblank counter */
    {0x800A4704u, 0x800A48B8u}, /* gp+0x190 control configuration */
    {0x800A4758u, 0x800A490Cu}, /* map */
    {0x800A476Eu, 0x800A4922u}, /* projection distance offset */
    {0x800A48C4u, 0x800A4A78u}, /* attract demo playback */
    {0x800A49E8u, 0x800A4B9Cu}, /* gp+0x474 player object */
    {0x800A49F0u, 0x800A4BA4u}, /* gp+0x47C inventory */
    {0x800A4A14u, 0x800A4BC8u}, /* section load flag */
    {0x800A7A44u, 0x800A7BF8u}, /* pickup amount table */
    {0x800A7B10u, 0x800A7CC4u}, /* flashlight capacity */
    {0x800A81BCu, 0x800A8370u}, /* screen-space quad subdivision (asm) */
    {0x800A86A4u, 0x800A8858u}, /* gp+0x4130 vblanks since the last frame */
    {0x800A86A8u, 0x800A885Cu}, /* gp+0x4134 vblank counter at this frame */
    {0x800A8758u, 0x800A890Cu}, /* ripple effect table */
    {0x800BDAF0u, 0x800BDCA4u}, /* processed pad 0 axes */
    {0x800BDBE0u, 0x800BDD94u}, /* command block: turn */
    {0x800BDBE4u, 0x800BDD98u}, /* command block: look */
    {0x800BDBF8u, 0x800BDDACu}, /* command block: look rate */
    {0x800BDC20u, 0x800BDDD4u}, /* command block: control variant A */
    {0x800BDC28u, 0x800BDDDCu}, /* command block: control variant B */
};

/* European video mode variables (0 NTSC, 1 PAL; 1 on the disc). The port
 * plays the European release at 60 Hz (NTSC), the way the USA one runs: in
 * PAL mode ALIEN.BIN draws a 256-line picture and scales its clock by 60/50
 * to make up for the 50 Hz refresh, and the port refreshes at 60 Hz, so PAL
 * mode would run 20% fast. Both are set to NTSC when their program loads. */
#define EU_ALIEN_VIDEO_MODE 0x800A4A30u /* ALIEN.BIN, read in 61 places */
#define EU_BOOT_VIDEO_MODE  0x80022C0Cu /* SLES_029.13 gp+4, SetVideoMode() */

/* Language. After the intro the European boot program takes the language
 * from the newest save on the memory card (its byte 17) into the word
 * below, or sets it to -1 and shows its language screen (LANGSEL.BIN:
 * Deutsch, Francais, English, Italiano, Espanol). It hands the choice to
 * ALIEN.BIN in the first word of the scratchpad (its gp+28 pointer). A
 * preset in that word skips the screen; started directly (skip intro),
 * ALIEN.BIN gets the language in the scratchpad. */
#define EU_BOOT_LANGUAGE    0x80022C3Cu /* SLES_029.13 gp+0x34 */
#define EU_LANGUAGE_HANDOFF 0x1F800000u
/* The memory card check sets that word after the program starts, so the
 * language screen's read of it (0x80010EC8, lw v1,0x2C3C(v1)) is replaced
 * with li v1,<language> when a language is set (the game code is run from
 * RAM by the interpreter for this release). */
#define EU_BOOT_LANGUAGE_READ    0x80010EC8u
#define EU_BOOT_LANGUAGE_READ_OP 0x8C632C3Cu

int g_eu_language = -1;
static bool g_boot_ran;

int region_language_code(const char *id) {
    static const char *const ids[] = {"en", "fr", "de", "it", "es"};
    for (int i = 0; i < 5; i++)
        if (id && !strcasecmp(id, ids[i]))
            return i;
    return -1;
}

uint32_t rg(uint32_t a) {
    if (g_region == REGION_USA)
        return a;
    uint32_t key = 0x80000000u | (a & 0x1FFFFFFFu);
    unsigned lo = 0, hi = sizeof k_eu / sizeof k_eu[0];
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (k_eu[mid].usa == key)
            return (a & 0xE0000000u) | (k_eu[mid].eu & 0x1FFFFFFFu);
        if (k_eu[mid].usa < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    static uint32_t warned[16];
    static unsigned nwarned;
    for (unsigned i = 0; i < nwarned; i++)
        if (warned[i] == key)
            return a;
    if (nwarned < 16)
        warned[nwarned++] = key;
    LOGW("region", "no European address for %08X", key);
    return a;
}

static const char *const k_boot[] = {"SLUS_006.33", "SLES_029.13"};
static const char *const k_name[] = {"Alien Resurrection (USA) - SLUS-00633",
                                     "Alien Resurrection (Europe) - SLES-02913"};

const char *region_boot_exe(void) { return k_boot[g_region]; }
const char *region_name(void) { return k_name[g_region]; }

bool region_from_cnf(const char *cnf) {
    for (int r = REGION_USA; r <= REGION_EUROPE; r++)
        if (strstr(cnf, k_boot[r])) {
            g_region = (GameRegion)r;
            return true;
        }
    return false;
}

void region_exe_loaded(const char *exe) {
    if (g_region != REGION_EUROPE)
        return;
    bool boot = !strcasecmp(exe, k_boot[REGION_EUROPE]);
    bool game = !strcasecmp(exe, "ALIEN.BIN");

    uint32_t var = game ? EU_ALIEN_VIDEO_MODE : boot ? EU_BOOT_VIDEO_MODE : 0;
    if (var && mem_r8(var) == 1) {
        mem_w8(var, 0);
        LOGI("region", "%s: video mode set to NTSC (60 Hz)", exe);
    }

    if (boot) {
        g_boot_ran = true;
        if (g_eu_language >= 0) {
            if (mem_r32(EU_BOOT_LANGUAGE_READ) == EU_BOOT_LANGUAGE_READ_OP) {
                mem_w32(EU_BOOT_LANGUAGE_READ, 0x24030000u | (uint32_t)g_eu_language);
                LOGI("region", "language %d (no language screen)", g_eu_language);
            } else {
                LOGW("region", "unexpected boot program code: language not set");
            }
        }
    } else if (game && g_eu_language >= 0 && !g_boot_ran) {
        /* started without the boot program: hand the language over as it would */
        mem_w32(EU_LANGUAGE_HANDOFF, (uint32_t)g_eu_language);
        LOGI("region", "language %d", g_eu_language);
    }
}
