/*
 * Shared definitions between the statically recompiled game code
 * (src/gen/...) and the native runtime.
 *
 * The recompiled code keeps the original MIPS register file in a CPU
 * context and accesses PS1 memory through the inline helpers below. Fast
 * paths cover main RAM and the scratchpad; everything else (hardware I/O)
 * goes through the runtime's bus.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Cycles charged per recompiled instruction, in 1/16 cycle units. The
 * R3000A at 33.8688 MHz averages roughly 2 cycles per instruction once
 * memory stalls are included (32). Lower values "overclock" the emulated
 * CPU so heavy scenes fit in one video frame (see --cpu-speed). */
extern uint32_t g_cycle_scale;

typedef struct GTE {
    uint32_t data[32];
    uint32_t ctrl[32];
} GTE;

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t pc;          /* last recorded pc (diagnostics only) */
    uint32_t cop0[32];    /* 12=SR, 13=CAUSE, 14=EPC */
    int64_t cycles;       /* elapsed CPU cycles */
    int64_t next_event;   /* cycle count at which the scheduler must run */
    uint32_t cycle_acc;   /* fractional cycles (1/16 units) */
    GTE gte;
} CPU;

typedef void (*RecompFunc)(CPU *c);
typedef struct FuncEntry {
    uint32_t addr;
    RecompFunc fn;
    const char *name;
} FuncEntry;
typedef struct ResumeEntry {
    uint32_t addr;
    void (*fn)(CPU *c, uint32_t at);
} ResumeEntry;

extern uint8_t g_ram[0x200000];
extern uint8_t g_scratch[0x400];

/* Debug store watch: stores to physical RAM in [g_watch_lo, g_watch_lo +
 * g_watch_len) are reported (see --watch). */
extern uint32_t g_watch_lo, g_watch_len;
void mem_watch_hit(uint32_t addr, uint32_t val, int size);
#define WATCH(p, v, sz)                                            \
    do {                                                           \
        if (unlikely(((p) & 0x1FFFFFu) - g_watch_lo < g_watch_len)) \
            mem_watch_hit((p), (v), (sz));                         \
    } while (0)

/* ---- slow-path bus (hardware registers, BIOS ROM, cache control) ---- */
uint32_t bus_read32(uint32_t addr);
uint32_t bus_read16(uint32_t addr);
uint32_t bus_read8(uint32_t addr);
void bus_write32(uint32_t addr, uint32_t v);
void bus_write16(uint32_t addr, uint32_t v);
void bus_write8(uint32_t addr, uint32_t v);

static inline uint32_t mem_r32(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    uint32_t v;
    if (likely(p < 0x00800000u && a < 0xC0000000u)) {
        memcpy(&v, &g_ram[p & 0x1FFFFCu], 4);
        return v;
    }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        memcpy(&v, &g_scratch[p & 0x3FCu], 4);
        return v;
    }
    return bus_read32(a);
}
static inline uint32_t mem_r16(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    uint16_t v;
    if (likely(p < 0x00800000u && a < 0xC0000000u)) {
        memcpy(&v, &g_ram[p & 0x1FFFFEu], 2);
        return v;
    }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        memcpy(&v, &g_scratch[p & 0x3FEu], 2);
        return v;
    }
    return bus_read16(a);
}
static inline uint32_t mem_r8(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (likely(p < 0x00800000u && a < 0xC0000000u))
        return g_ram[p & 0x1FFFFFu];
    if ((p & 0xFFFFFC00u) == 0x1F800000u)
        return g_scratch[p & 0x3FFu];
    return bus_read8(a);
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (likely(p < 0x00800000u && a < 0xC0000000u)) {
        WATCH(p, v, 4);
        memcpy(&g_ram[p & 0x1FFFFCu], &v, 4);
        return;
    }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        memcpy(&g_scratch[p & 0x3FCu], &v, 4);
        return;
    }
    bus_write32(a, v);
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    uint16_t h = (uint16_t)v;
    if (likely(p < 0x00800000u && a < 0xC0000000u)) {
        WATCH(p, v, 2);
        memcpy(&g_ram[p & 0x1FFFFEu], &h, 2);
        return;
    }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        memcpy(&g_scratch[p & 0x3FEu], &h, 2);
        return;
    }
    bus_write16(a, v);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (likely(p < 0x00800000u && a < 0xC0000000u)) {
        WATCH(p, v, 1);
        g_ram[p & 0x1FFFFFu] = (uint8_t)v;
        return;
    }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        g_scratch[p & 0x3FFu] = (uint8_t)v;
        return;
    }
    bus_write8(a, v);
}

/* Unaligned load/store helpers (little-endian R3000 semantics). */
static inline uint32_t mem_lwl(uint32_t rt, uint32_t a) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00FFFFFFu) | (w << 24);
    case 1: return (rt & 0x0000FFFFu) | (w << 16);
    case 2: return (rt & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
static inline uint32_t mem_lwr(uint32_t rt, uint32_t a) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xFF000000u) | (w >> 8);
    case 2: return (rt & 0xFFFF0000u) | (w >> 16);
    default: return (rt & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void mem_swl(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (rt >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (rt >> 16); break;
    case 2: w = (w & 0xFF000000u) | (rt >> 8); break;
    default: w = rt; break;
    }
    mem_w32(al, w);
}
static inline void mem_swr(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000FFu) | (rt << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (rt << 16); break;
    default: w = (w & 0x00FFFFFFu) | (rt << 24); break;
    }
    mem_w32(al, w);
}

/* ---- CPU helpers ---- */
static inline void cpu_div(CPU *c, uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) {
        c->hi = a;
        c->lo = (n >= 0) ? 0xFFFFFFFFu : 1u;
    } else if (a == 0x80000000u && d == -1) {
        c->hi = 0;
        c->lo = 0x80000000u;
    } else {
        c->lo = (uint32_t)(n / d);
        c->hi = (uint32_t)(n % d);
    }
}
static inline void cpu_divu(CPU *c, uint32_t a, uint32_t b) {
    if (b == 0) {
        c->hi = a;
        c->lo = 0xFFFFFFFFu;
    } else {
        c->lo = a / b;
        c->hi = a % b;
    }
}

/* ---- runtime services used by generated code ---- */
void cpu_service_events(CPU *c);
void call_indirect(CPU *c, uint32_t addr);
void jump_indirect(CPU *c, uint32_t addr);
void bad_jump(CPU *c, uint32_t pc, uint32_t target);
void missing_label(CPU *c, uint32_t addr);
void ret_mismatch(CPU *c, uint32_t ra, uint32_t expected);
void hle_syscall(CPU *c, uint32_t code);
void hle_break(CPU *c, uint32_t pc, uint32_t code);
void hle_invalid(CPU *c, uint32_t pc, uint32_t word);
void bios_call_A0(CPU *c);
void bios_call_B0(CPU *c);
void bios_call_C0(CPU *c);
uint32_t cop0_read(CPU *c, int reg);
void cop0_write(CPU *c, int reg, uint32_t v);
void cop0_rfe(CPU *c);
uint32_t gte_read_data(CPU *c, int reg);
uint32_t gte_read_ctrl(CPU *c, int reg);
void gte_write_data(CPU *c, int reg, uint32_t v);
void gte_write_ctrl(CPU *c, int reg, uint32_t v);
void gte_command(CPU *c, uint32_t cmd);

/* PGXP precision tracking hooks (see runtime/pgxp.h) */
extern _Bool g_pgxp;
void pgxp_mfc2(int rt, int rd);
void pgxp_mtc2(int rd, int rt);
void pgxp_swc2(uint32_t addr, int rd);
void pgxp_lwc2(int rd, uint32_t addr);
void pgxp_lw(int rt, uint32_t addr);
void pgxp_sw(uint32_t addr, int rt);
void pgxp_mov(int rd, int rs);
#define PGXP_LW(rt, a) do { if (unlikely(g_pgxp)) pgxp_lw((rt), (a)); } while (0)
#define PGXP_SW(a, rt) do { if (unlikely(g_pgxp)) pgxp_sw((a), (rt)); } while (0)
#define PGXP_MOV(rd, rs) do { if (unlikely(g_pgxp)) pgxp_mov((rd), (rs)); } while (0)
#define PGXP_MFC2(rt, rd) do { if (unlikely(g_pgxp)) pgxp_mfc2((rt), (rd)); } while (0)
#define PGXP_MTC2(rd, rt) do { if (unlikely(g_pgxp)) pgxp_mtc2((rd), (rt)); } while (0)
#define PGXP_SWC2(a, rd) do { if (unlikely(g_pgxp)) pgxp_swc2((a), (rd)); } while (0)
#define PGXP_LWC2(rd, a) do { if (unlikely(g_pgxp)) pgxp_lwc2((rd), (a)); } while (0)

/* Lockstep/trace hooks (see runtime/verify.c). Enabled at build time. */
#ifdef RECOMP_TRACE
void trace_block(CPU *c, uint32_t addr);
void trace_enter(CPU *c, uint32_t addr);
#define TRACE_BLOCK(a) trace_block(c, (a))
#else
/* record the block address for diagnostics (one store per block) */
#define TRACE_BLOCK(a) (c->pc = (a))
#endif

/* ---- macros consumed by the generated code ---- */
#define R(n) (c->r[n])
#define MEM_R8 mem_r8
#define MEM_R16 mem_r16
#define MEM_R32 mem_r32
#define MEM_W8 mem_w8
#define MEM_W16 mem_w16
#define MEM_W32 mem_w32
#define ADD_CYCLES(n)                                 \
    do {                                              \
        c->cycle_acc += (uint32_t)(n) * g_cycle_scale; \
        c->cycles += c->cycle_acc >> 4;               \
        c->cycle_acc &= 15;                           \
    } while (0)
#define CHECK_IRQ()                                   \
    do {                                              \
        if (unlikely(c->cycles >= c->next_event))     \
            cpu_service_events(c);                    \
    } while (0)
extern int g_verify_count;
int verify_hook(CPU *c, uint32_t addr, RecompFunc fn);
#define FUNC_ENTER(addr, self)                                        \
    const uint32_t entry_ra_ = c->r[31];                              \
    if (unlikely(g_verify_count) && verify_hook(c, (addr), (self)))   \
        return
#define FUNC_ENTER_RESUMABLE(at)                      \
    const uint32_t entry_ra_ = (at) ? 0xFFFFFFFFu : c->r[31]
#define RETURN(ra)                                                       \
    do {                                                                 \
        if (unlikely((ra) != entry_ra_) && entry_ra_ != 0xFFFFFFFFu)     \
            ret_mismatch(c, (ra), entry_ra_);                            \
        return;                                                          \
    } while (0)

#ifdef __cplusplus
}
#endif
