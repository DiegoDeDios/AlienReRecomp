/*
 * Lockstep verification of recompiled functions against the reference
 * interpreter.
 *
 * When a function selected with --verify ADDR is entered, the CPU state and
 * RAM are snapshotted; the function runs once fully interpreted and once as
 * recompiled C from the same snapshot. Registers and memory are compared
 * and differences reported. Hardware side effects are not replayed, so this
 * is intended for computational code (decoders, math, game logic).
 */
#include <stdlib.h>

#include "psx.h"

uint32_t g_verify_addr[16];
int g_verify_count;
int g_verify_limit = 8;   /* number of calls to verify per function */
static int g_in_verify;
static int g_verified[16];
static int g_failures;

extern bool g_interp_call_native;
void interp_run(CPU *c, uint32_t pc, uint32_t stop_ra);

static uint8_t *snap_ram, *res_ram;

int verify_hook(CPU *c, uint32_t addr, RecompFunc fn) {
    if (g_in_verify)
        return 0;
    int slot = -1;
    for (int i = 0; i < g_verify_count; i++)
        if (g_verify_addr[i] == addr)
            slot = i;
    if (slot < 0 || g_verified[slot] >= g_verify_limit)
        return 0;
    g_verified[slot]++;
    g_in_verify = 1;
    if (!snap_ram) {
        snap_ram = malloc(sizeof g_ram + sizeof g_scratch);
        res_ram = malloc(sizeof g_ram + sizeof g_scratch);
    }
    /* snapshot */
    CPU s0 = *c;
    memcpy(snap_ram, g_ram, sizeof g_ram);
    memcpy(snap_ram + sizeof g_ram, g_scratch, sizeof g_scratch);
    int64_t saved_next = c->next_event;

    /* 1. reference interpreter, no events, everything interpreted */
    c->next_event = INT64_MAX;
    bool saved_native = g_interp_call_native;
    g_interp_call_native = false;
    interp_run(c, addr, c->r[31]);
    g_interp_call_native = saved_native;
    CPU s1 = *c;
    memcpy(res_ram, g_ram, sizeof g_ram);
    memcpy(res_ram + sizeof g_ram, g_scratch, sizeof g_scratch);

    /* 2. recompiled code from the same snapshot */
    *c = s0;
    c->next_event = INT64_MAX;
    memcpy(g_ram, snap_ram, sizeof g_ram);
    memcpy(g_scratch, snap_ram + sizeof g_ram, sizeof g_scratch);
    fn(c);
    c->next_event = saved_next;

    /* 3. compare */
    int diffs = 0;
    static const char *rn[32] = {
        "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    for (int r = 1; r < 32; r++) {
        if (s1.r[r] != c->r[r]) {
            /* caller-saved temporaries may legitimately differ only if the
             * code is buggy; report everything */
            LOGW("verify", "%08X: %s interp=%08X recomp=%08X", addr, rn[r],
                 s1.r[r], c->r[r]);
            diffs++;
        }
    }
    if (s1.hi != c->hi || s1.lo != c->lo) {
        LOGW("verify", "%08X: hi/lo interp=%08X/%08X recomp=%08X/%08X", addr,
             s1.hi, s1.lo, c->hi, c->lo);
        diffs++;
    }
    int mem_diffs = 0;
    for (size_t i = 0; i < sizeof g_ram; i++) {
        if (g_ram[i] != res_ram[i]) {
            if (mem_diffs < 16)
                LOGW("verify", "%08X: RAM[%06zX] interp=%02X recomp=%02X",
                     addr, i, res_ram[i], g_ram[i]);
            mem_diffs++;
        }
    }
    for (size_t i = 0; i < sizeof g_scratch; i++) {
        if (g_scratch[i] != res_ram[sizeof g_ram + i]) {
            if (mem_diffs < 16)
                LOGW("verify", "%08X: SPAD[%03zX] interp=%02X recomp=%02X",
                     addr, i, res_ram[sizeof g_ram + i], g_scratch[i]);
            mem_diffs++;
        }
    }
    diffs += mem_diffs;
    if (diffs) {
        g_failures++;
        LOGE("verify", "%08X (%s) call #%d: %d differences (%d memory bytes)",
             addr, module_func_name(addr) ? module_func_name(addr) : "?",
             g_verified[slot], diffs, mem_diffs);
    } else {
        LOGI("verify", "%08X (%s) call #%d: identical", addr,
             module_func_name(addr) ? module_func_name(addr) : "?",
             g_verified[slot]);
    }
    g_in_verify = 0;
    return 1;
}
