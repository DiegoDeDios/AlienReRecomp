/*
 * Save states (quick save / quick load): a snapshot of the whole emulated
 * machine, taken and restored at a safe point in the game's main thread
 * (see savestate.c).
 *
 * Every module serializes its state with one function that both writes
 * and reads (StateIO.loading), so the two directions cannot drift apart:
 *
 *     void timers_state(StateIO *s) { state_section(s, "TIMR"); STATE(s, g_t); }
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StateIO {
    uint8_t *buf;
    size_t pos, size, cap;
    bool loading;
    bool error;
} StateIO;

/* copy n bytes between p and the state (direction from s->loading) */
void state_io(StateIO *s, void *p, size_t n);
#define STATE(s, v) state_io((s), &(v), sizeof(v))
/* a tag that must match on load: catches a module reading the wrong data */
void state_section(StateIO *s, const char *tag4);
/* a heap buffer of `*len` elements of `elem` bytes; reallocated on load */
void state_heap(StateIO *s, void **p, size_t *len, size_t *cap, size_t elem);

/* per-module serializers (in savestate.c's order) */
void cpu_state(StateIO *s);
void bus_state(StateIO *s);
void bios_state(StateIO *s);
void interp_state(StateIO *s);
void dma_state(StateIO *s);
void timers_state(StateIO *s);
void gpu_state(StateIO *s);
void cdrom_state(StateIO *s);
void spu_state(StateIO *s);
void mdec_state(StateIO *s);
void sio_state(StateIO *s);
void ws_state(StateIO *s);
void platform_state(StateIO *s); /* frontend: frame counter */

/* Requests from the frontend, carried out at the next safe point (the
 * game's next wait for vblank). The result is reported through
 * platform_state_done. */
void state_request_save(const char *path);
void state_request_load(const char *path);
bool state_request_pending(void);
/* once per vblank: gives up on a request that found no safe point */
void state_tick(void);
void platform_state_done(bool save, bool ok, const char *message);
/* after a successful load: drop host-side leftovers (audio, pacing) */
void platform_state_loaded(void);

/* The safe point: called on entry to libetc VSync (native.c) */
void state_safe_point(CPU *c);

/* Bookkeeping the safe point needs from the CPU glue and interpreter */
int cpu_exception_depth(void);
uint32_t bios_stack_top(void);
bool bios_card_busy(void); /* a memory card file is open or an operation pending */
/* native overrides being run by the interpreter (innermost last) */
extern uint32_t g_ov_chain[16];
extern int g_ov_depth;
/* incremented by every successful load (the interpreter then continues at
 * the restored return address) */
extern unsigned g_state_loads;

#ifdef __cplusplus
}
#endif
