/*
 * Game releases the port runs: the USA disc (SLUS-00633) and the European
 * one (SLES-02913). Both share the main game program ALIEN.BIN, built from
 * the same source; the European build has its code and data at slightly
 * different addresses. The native code (native.c, automap.c, widescreen.c)
 * and the override lists (tools/overrides_*.txt) name the USA addresses;
 * rg() translates them for the disc in the drive.
 */
#ifndef REGION_H
#define REGION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { REGION_USA, REGION_EUROPE } GameRegion;

extern GameRegion g_region;

/* Play the European release as a 60 Hz (NTSC) game, the way the USA one
 * runs: on by default. The European ALIEN.BIN supports both video modes
 * from a variable that is always PAL on that disc; in PAL mode it draws a
 * 256-line picture and scales its clock by 60/50 to make up for the 50 Hz
 * refresh. The port refreshes at 60 Hz, so PAL mode would run 20% fast. */
extern bool g_eu_ntsc;

/* European disc language, the game's code: 0 English, 1 French, 2 German,
 * 3 Italian, 4 Spanish; -1 = let the game ask (its language screen after
 * the intro, or the language of the save on the memory card). */
extern int g_eu_language;

/* "en", "fr", "de", "it", "es" -> the code above, otherwise -1 */
int region_language_code(const char *id);

/* Identify the release from SYSTEM.CNF of the open disc. Returns false
 * (and a message) when it is neither. */
bool region_detect(char *msg, size_t msg_n);

/* executable the disc boots (SYSTEM.CNF BOOT), e.g. "SLUS_006.33" */
const char *region_boot_exe(void);

/* short description, e.g. "Alien Resurrection (Europe) - SLES-02913" */
const char *region_name(void);

/* USA address -> the same code or data in this release. Keeps the segment
 * bits (KUSEG/KSEG0) of the argument; unknown addresses are returned as
 * they are (and logged once). */
uint32_t rg(uint32_t usa_addr);

/* called after an executable is loaded into RAM (before it runs) */
void region_exe_loaded(const char *exe);

#ifdef __cplusplus
}
#endif

#endif
