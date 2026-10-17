/*
 * CPU glue for recompiled code: function dispatch, the event scheduler,
 * interrupt entry/exit and COP0.
 */
#include <stdarg.h>
#include <stdlib.h>

#include "psx.h"

CPU *g_cpu;
int g_log_level = LOG_INFO;
uint32_t g_watch_lo, g_watch_len;
/* Default: 3x the original CPU throughput (about 0.67 cycles/insn), which
 * keeps the game at a steady 60 fps in heavy scenes. The game's logic is
 * scaled by elapsed vblanks, so this only removes slowdowns.
 * --cpu-speed 100 restores original timing (2 cycles/insn = 32). */
uint32_t g_cycle_scale = 32 * 100 / 300;

void mem_watch_hit(uint32_t addr, uint32_t val, int size) {
    LOGI("watch", "store%d [%08X] = %08X  pc~%08X ra=%08X frame-cycles=%lld",
         size * 8, addr, val, g_cpu->pc, g_cpu->r[31], (long long)g_cpu->cycles);
}

void log_msg(int level, const char *sys, const char *fmt, ...) {
    static const char *lv[] = {"E", "W", "I", "D"};
    if (level > g_log_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s %-5s] ", lv[level], sys);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void dump_state(CPU *c) {
    static const char *rn[32] = {
        "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    fprintf(stderr, "pc=%08X cycles=%lld\n", c->pc, (long long)c->cycles);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "%s=%08X%s", rn[i], c->r[i], (i & 7) == 7 ? "\n" : " ");
}

_Noreturn void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (g_cpu)
        dump_state(g_cpu);
    abort();
}

/* ------------------------------------------------------------------ */
/* Modules and dispatch                                                */

extern const FuncEntry alien_func_table[];
extern const unsigned alien_func_count;
extern const ResumeEntry alien_resume_table[];
extern const unsigned alien_resume_count;
extern const FuncEntry slus_func_table[];
extern const unsigned slus_func_count;
extern const ResumeEntry slus_resume_table[];
extern const unsigned slus_resume_count;

static Module g_modules[2];
static const Module *g_mod;

static void modules_setup(void) {
    if (g_modules[0].name)
        return;
    g_modules[0] = (Module){"ALIEN.BIN", alien_func_table, alien_func_count,
                            alien_resume_table, alien_resume_count};
    g_modules[1] = (Module){"SLUS_006.33", slus_func_table, slus_func_count,
                            slus_resume_table, slus_resume_count};
}

const Module *module_find_by_exe(const char *name) {
    modules_setup();
    for (int i = 0; i < 2; i++)
        if (!strcasecmp(g_modules[i].name, name))
            return &g_modules[i];
    return NULL;
}

void module_activate(const Module *m) {
    g_mod = m;
    LOGI("cpu", "active module: %s (%u functions)", m->name, m->nfuncs);
}

const Module *module_current(void) { return g_mod; }

static const FuncEntry *find_entry(uint32_t addr) {
    if (!g_mod)
        return NULL;
    uint32_t a = 0x80000000u | (addr & 0x1FFFFFFFu);
    unsigned lo = 0, hi = g_mod->nfuncs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t m = g_mod->funcs[mid].addr;
        if (m == a)
            return &g_mod->funcs[mid];
        if (m < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

RecompFunc module_lookup(uint32_t addr) {
    const FuncEntry *e = find_entry(addr);
    return e ? e->fn : NULL;
}

const char *module_func_name(uint32_t addr) {
    const FuncEntry *e = find_entry(addr);
    return (e && e->name && e->name[0]) ? e->name : NULL;
}

/* Interpreter fallback (runtime/interp.c) */
void interp_call(CPU *c, uint32_t addr);

void call_indirect(CPU *c, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p == 0xA0) { bios_call_A0(c); return; }
    if (p == 0xB0) { bios_call_B0(c); return; }
    if (p == 0xC0) { bios_call_C0(c); return; }
    RecompFunc f = module_lookup(addr);
    if (likely(f != NULL)) {
        f(c);
        return;
    }
    LOGW("cpu", "call to unknown address %08X (ra=%08X), interpreting", addr,
         c->r[31]);
    interp_call(c, addr);
}

void jump_indirect(CPU *c, uint32_t addr) {
    /* Tail jump through a register: behaves like a call that returns to
     * the current ra. */
    call_indirect(c, addr);
}

void guest_call(CPU *c, uint32_t addr) {
    uint32_t saved_ra = c->r[31];
    c->r[31] = 0xFFFFFFF0u; /* sentinel; callee returns to us */
    call_indirect(c, addr);
    c->r[31] = saved_ra;
}

void guest_resume(CPU *c, uint32_t addr) {
    uint32_t a = 0x80000000u | (addr & 0x1FFFFFFFu);
    if (g_mod) {
        for (unsigned i = 0; i < g_mod->nresume; i++) {
            if (g_mod->resume[i].addr == a) {
                g_mod->resume[i].fn(c, a);
                return;
            }
        }
    }
    RecompFunc f = module_lookup(a);
    if (f) {
        f(c);
        return;
    }
    LOGW("cpu", "resume at %08X: no entry, interpreting", a);
    interp_call(c, a);
}

void bad_jump(CPU *c, uint32_t pc, uint32_t target) {
    c->pc = pc;
    fatal("bad jump-table target %08X at %08X", target, pc);
}

void missing_label(CPU *c, uint32_t addr) {
    c->pc = addr;
    fatal("jump to code not covered by recompiled function: %08X", addr);
}

void ret_mismatch(CPU *c, uint32_t ra, uint32_t expected) {
    /* Only a warning: some code legitimately returns to a different
     * address (e.g. exception trampolines). */
    static int count;
    if (count++ < 32)
        LOGW("cpu", "return to %08X but entered with ra=%08X (pc~%08X)", ra,
             expected, c->pc);
}

void hle_break(CPU *c, uint32_t pc, uint32_t code) {
    c->pc = pc;
    fatal("BREAK %X at %08X", code, pc);
}

void hle_invalid(CPU *c, uint32_t pc, uint32_t word) {
    c->pc = pc;
    fatal("invalid instruction %08X at %08X", word, pc);
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */

static struct {
    int64_t when;
    EventFn fn;
    bool active;
} g_events[EV_COUNT];

void sched_init(void) { memset(g_events, 0, sizeof g_events); }
void sched_register(EventId id, EventFn fn) { g_events[id].fn = fn; }

void sched_at(EventId id, int64_t when) {
    g_events[id].when = when;
    g_events[id].active = true;
    if (g_cpu && when < g_cpu->next_event)
        g_cpu->next_event = when;
}
void sched_in(EventId id, int64_t delta) { sched_at(id, now_cycles() + delta); }
void sched_cancel(EventId id) { g_events[id].active = false; }
bool sched_pending(EventId id) { return g_events[id].active; }
int64_t sched_time(EventId id) { return g_events[id].when; }

int64_t sched_next(void) {
    int64_t n = INT64_MAX;
    for (int i = 0; i < EV_COUNT; i++)
        if (g_events[i].active && g_events[i].when < n)
            n = g_events[i].when;
    return n;
}

void sched_run(int64_t now) {
    for (;;) {
        int best = -1;
        int64_t bw = INT64_MAX;
        for (int i = 0; i < EV_COUNT; i++) {
            if (g_events[i].active && g_events[i].when <= now &&
                g_events[i].when < bw) {
                bw = g_events[i].when;
                best = i;
            }
        }
        if (best < 0)
            break;
        g_events[best].active = false;
        if (g_events[best].fn)
            g_events[best].fn(bw, now - bw);
    }
}

void cpu_request_check(void) {
    if (g_cpu)
        g_cpu->next_event = g_cpu->cycles;
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */

uint32_t g_istat, g_imask;

void irq_raise(int n) {
    g_istat |= 1u << n;
    cpu_request_check();
}

static bool irq_deliverable(CPU *c) {
    uint32_t sr = c->cop0[12];
    return (g_istat & g_imask & 0x7FF) && (sr & 1) && (sr & 0x400);
}

void cpu_service_events(CPU *c) {
    sched_run(c->cycles);
    c->next_event = sched_next();
    if (irq_deliverable(c))
        cpu_take_interrupt(c);
}

static host_jmp_buf *g_exc_jmp;
static int g_exc_depth;

void cpu_take_interrupt(CPU *c) {
    LOGD("irq", "take interrupt istat=%03X imask=%03X pc~%08X", g_istat, g_imask, c->pc);
    uint32_t saved_r[32];
    uint32_t saved_hi = c->hi, saved_lo = c->lo, saved_pc = c->pc;
    memcpy(saved_r, c->r, sizeof saved_r);

    /* exception entry: push the KU/IE stack, record cause */
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);
    c->cop0[13] = (c->cop0[13] & ~0x7Cu) | (1u << 10); /* ExcCode=Int, IP2 */
    c->cop0[14] = c->pc;

    host_jmp_buf jb;
    host_jmp_buf *prev = g_exc_jmp;
    g_exc_jmp = &jb;
    g_exc_depth++;
    if (host_setjmp(jb) == 0) {
        bios_exception(c);
        /* bios_exception returning means no ReturnFromException was
         * executed by guest code; treat as an implicit one. */
    }
    g_exc_depth--;
    g_exc_jmp = prev;

    memcpy(c->r, saved_r, sizeof saved_r);
    c->hi = saved_hi;
    c->lo = saved_lo;
    c->pc = saved_pc;
    /* rfe: pop the KU/IE stack */
    sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
    c->cop0[13] &= ~(1u << 10);
    cpu_request_check();
}

void cpu_return_from_exception(CPU *c) {
    (void)c;
    if (!g_exc_jmp)
        fatal("ReturnFromException outside of an exception");
    host_longjmp(*g_exc_jmp);
}

/* ------------------------------------------------------------------ */
/* COP0                                                                */

uint32_t cop0_read(CPU *c, int reg) {
    switch (reg) {
    case 12: return c->cop0[12];
    case 13: {
        uint32_t cause = c->cop0[13] & ~(1u << 10);
        if (g_istat & g_imask)
            cause |= 1u << 10;
        return cause;
    }
    case 14: return c->cop0[14];
    case 15: return 0x00000002u; /* PRId */
    default: return c->cop0[reg];
    }
}

void cop0_write(CPU *c, int reg, uint32_t v) {
    if (reg == 13) {
        /* only the software interrupt bits are writable */
        c->cop0[13] = (c->cop0[13] & ~0x300u) | (v & 0x300u);
    } else {
        c->cop0[reg] = v;
    }
    cpu_request_check();
}

void cop0_rfe(CPU *c) {
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
    cpu_request_check();
}

/* syscall: EnterCriticalSection (a0=1) / ExitCriticalSection (a0=2) /
 * ChangeThreadSubFunction (a0=3, unused). */
void hle_syscall(CPU *c, uint32_t code) {
    (void)code;
    uint32_t sr = c->cop0[12];
    switch (c->r[4]) {
    case 0: /* NoFunction */
        break;
    case 1: /* EnterCriticalSection: returns 1 if interrupts were enabled.
             * The BIOS clears IEp+IM2 in the saved SR; outside of a real
             * exception frame that is IEc+IM2. */
        c->r[2] = ((sr & 0x401) == 0x401) ? 1 : 0;
        c->cop0[12] = sr & ~0x401u;
        break;
    case 2: /* ExitCriticalSection */
        c->cop0[12] = sr | 0x401u;
        break;
    default:
        LOGW("cpu", "syscall a0=%u", c->r[4]);
        break;
    }
    cpu_request_check();
}
