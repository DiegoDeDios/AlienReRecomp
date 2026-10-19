/*
 * High-level emulation of the PS1 BIOS (kernel) services used by the game:
 * A0/B0/C0 function tables, events, interrupt chains/hooks, heap, file I/O
 * on the memory card ("bu00:"), LoadExec and console output.
 */
#include <ctype.h>
#include <stdlib.h>

#include "psx.h"
#include "iso.h"
#include "memcard.h"
#include "savestate.h"

#define A0 4
#define A1 5
#define A2 6
#define A3 7
#define V0 2
#define SP 29

/* ------------------------------------------------------------------ */
/* guest memory helpers                                                */

static inline uint8_t *ram_ptr(uint32_t a) { return &g_ram[a & 0x1FFFFF]; }
static inline uint32_t rd32(uint32_t a) { return mem_r32(a); }
static inline void wr32(uint32_t a, uint32_t v) { mem_w32(a, v); }

static size_t guest_strlen(uint32_t a) {
    size_t n = 0;
    while (mem_r8(a + n))
        n++;
    return n;
}

static void guest_cstr(uint32_t a, char *buf, size_t max) {
    size_t i = 0;
    for (; i + 1 < max; i++) {
        char ch = (char)mem_r8(a + i);
        buf[i] = ch;
        if (!ch)
            return;
    }
    buf[i] = 0;
}

/* ------------------------------------------------------------------ */
/* kernel state                                                        */

#define KERNEL_A0_TABLE 0x00000200u
#define KERNEL_B0_TABLE 0x00000874u
#define KERNEL_C0_TABLE 0x00000674u
#define KERNEL_DUMMY 0x00000C80u
/* Device control blocks: [0x150] = table, [0x154] = size in bytes. PsyQ's
 * libapi firstfile() looks the device up there (it temporarily patches the
 * device's firstfile handler at +0x34) and returns "no files" without
 * calling B(42h) when the device is missing. */
#define KERNEL_DCB_PTR 0x00000150u
#define KERNEL_DCB_SIZE 0x00000154u
#define KERNEL_DCB_TABLE 0x00001000u /* 10 entries of 0x50 bytes */
#define KERNEL_DCB_NAMES 0x00001400u
#define DCB_ENTRY 0x50u
#define DCB_MAX 10u

typedef struct {
    uint32_t cls, spec, mode, func;
    uint32_t status; /* 0=free, 0x1000 disabled, 0x2000 enabled, 0x4000 ready */
} EvCB;
#define MAX_EVENTS 32
static EvCB g_ev[MAX_EVENTS];

static uint32_t g_hook_jmpbuf;      /* HookEntryInt buffer, 0 = none */
static uint32_t g_chain[4];         /* SysEnqIntRP heads per priority */
static uint32_t g_heap_start, g_heap_end;
static uint32_t g_rand_seed = 0x24040001;
bool g_bios_exit_requested;

static host_jmp_buf g_boot_jmp;
static char g_pending_exec[128];
static uint32_t g_stack_top; /* the running executable's initial sp */

uint32_t bios_stack_top(void) { return g_stack_top; }

/* deferred event delivery (memory card completion etc.) */
typedef struct { uint32_t cls, spec; int64_t when; } PendingEv;
static PendingEv g_pending[16];
static int g_npending;

static void deliver_event(CPU *c, uint32_t cls, uint32_t spec);

static void bios_deferred_tick(int64_t now, int64_t late) {
    (void)late;
    int keep = 0;
    int64_t next = INT64_MAX;
    for (int i = 0; i < g_npending; i++) {
        if (g_pending[i].when <= now) {
            deliver_event(g_cpu, g_pending[i].cls, g_pending[i].spec);
        } else {
            if (g_pending[i].when < next)
                next = g_pending[i].when;
            g_pending[keep++] = g_pending[i];
        }
    }
    g_npending = keep;
    if (keep)
        sched_at(EV_BIOS, next);
}

static void defer_event(uint32_t cls, uint32_t spec, int64_t delay) {
    if (g_npending >= 16) {
        deliver_event(g_cpu, cls, spec);
        return;
    }
    g_pending[g_npending++] = (PendingEv){cls, spec, now_cycles() + delay};
    if (!sched_pending(EV_BIOS) || sched_time(EV_BIOS) > now_cycles() + delay)
        sched_at(EV_BIOS, now_cycles() + delay);
}

/* ------------------------------------------------------------------ */
/* events                                                              */

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode,
                           uint32_t func) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (g_ev[i].status == 0) {
            g_ev[i] = (EvCB){cls, spec, mode, func, 0x1000};
            LOGD("bios", "OpenEvent(%08X,%04X,%04X,%08X) = %d", cls, spec,
                 mode, func, i);
            return 0xF1000000u | (uint32_t)i;
        }
    }
    LOGE("bios", "OpenEvent: out of event slots");
    return 0xFFFFFFFFu;
}

static EvCB *ev_get(uint32_t h) {
    uint32_t i = h & 0xFFFF;
    if ((h & 0xFFFF0000u) != 0xF1000000u || i >= MAX_EVENTS)
        return NULL;
    return &g_ev[i];
}

static void deliver_event(CPU *c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        EvCB *e = &g_ev[i];
        if (e->status != 0x2000 || e->cls != cls || e->spec != spec)
            continue;
        if (e->mode == 0x2000) {
            e->status = 0x4000;
        } else if (e->mode == 0x1000 && e->func) {
            guest_call(c, e->func);
        }
    }
}

/* ------------------------------------------------------------------ */
/* heap (BIOS malloc)                                                  */
/* block header: [size | used] at addr-8, [magic] at addr-4 */

static void heap_init(uint32_t start, uint32_t size) {
    g_heap_start = (start + 7) & ~7u;
    g_heap_end = start + size;
    wr32(g_heap_start, (g_heap_end - g_heap_start) | 0); /* free block */
    wr32(g_heap_start + 4, 0x48454150u);
    LOGI("bios", "InitHeap(%08X, %X)", start, size);
}

static uint32_t heap_alloc(uint32_t size) {
    size = (size + 7) & ~7u;
    uint32_t need = size + 8;
    uint32_t p = g_heap_start;
    while (p + 8 <= g_heap_end) {
        uint32_t hdr = rd32(p);
        uint32_t bsz = hdr & ~7u;
        if (bsz == 0)
            break;
        if (!(hdr & 1)) {
            /* coalesce following free blocks */
            uint32_t q = p + bsz;
            while (q + 8 <= g_heap_end && !(rd32(q) & 1) && (rd32(q) & ~7u)) {
                bsz += rd32(q) & ~7u;
                q = p + bsz;
            }
            wr32(p, bsz);
            if (bsz >= need) {
                if (bsz - need >= 16) {
                    wr32(p + need, bsz - need);
                    wr32(p + need + 4, 0x48454150u);
                    bsz = need;
                }
                wr32(p, bsz | 1);
                return p + 8;
            }
        }
        p += bsz;
    }
    LOGE("bios", "malloc(%u) failed", size);
    return 0;
}

static void heap_free(uint32_t a) {
    if (!a)
        return;
    uint32_t p = (a & 0x1FFFFF) - 8;
    if ((p | 0x80000000u) < (g_heap_start | 0x80000000u))
        p = a - 8;
    uint32_t hdr = rd32(a - 8);
    wr32(a - 8, hdr & ~1u);
}

/* ------------------------------------------------------------------ */
/* printf                                                              */

typedef struct {
    CPU *c;
    int argi;
} VaState;

static uint32_t va_next(VaState *v) {
    int i = v->argi++;
    if (i < 4)
        return v->c->r[A0 + i];
    return rd32(v->c->r[SP] + 4u * (uint32_t)i);
}

static int guest_format(CPU *c, uint32_t fmt, int first_arg, char *out,
                        size_t outsz) {
    VaState va = {c, first_arg};
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < outsz) out[o++] = (char)(ch); } while (0)
    for (uint32_t p = fmt;; p++) {
        char ch = (char)mem_r8(p);
        if (!ch)
            break;
        if (ch != '%') {
            PUT(ch);
            continue;
        }
        char spec[32];
        int sl = 0;
        spec[sl++] = '%';
        p++;
        for (;;) {
            ch = (char)mem_r8(p);
            if (strchr("-+ #0123456789.", ch) && sl < 24) {
                spec[sl++] = ch;
                p++;
            } else if (ch == 'l' || ch == 'h') {
                p++;
            } else {
                break;
            }
        }
        char tmp[512];
        switch (ch) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            spec[sl++] = ch;
            spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)va_next(&va));
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "%08x", va_next(&va));
            break;
        case 's': {
            char s[256];
            uint32_t sa = va_next(&va);
            if (sa)
                guest_cstr(sa, s, sizeof s);
            else
                strcpy(s, "(null)");
            spec[sl++] = 's';
            spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, s);
            break;
        }
        case '%':
            strcpy(tmp, "%");
            break;
        case 0:
            tmp[0] = 0;
            p--;
            break;
        default:
            snprintf(tmp, sizeof tmp, "%%%c", ch);
            break;
        }
        for (char *t = tmp; *t; t++)
            PUT(*t);
    }
    out[o] = 0;
#undef PUT
    return (int)o;
}

static void guest_print(const char *s) {
    static char line[1024];
    static size_t ll;
    for (; *s; s++) {
        if (*s == '\n' || ll + 1 >= sizeof line) {
            line[ll] = 0;
            LOGI("guest", "%s", line);
            ll = 0;
        } else {
            line[ll++] = *s;
        }
    }
}

/* ------------------------------------------------------------------ */
/* executable loading                                                  */

static bool load_exe_bytes(CPU *c, const uint8_t *data, size_t len) {
    if (len < 0x800 || memcmp(data, "PS-X EXE", 8))
        return false;
    uint32_t h[10];
    memcpy(h, data + 0x10, sizeof h);
    uint32_t pc0 = h[0], gp0 = h[1], t_addr = h[2], t_size = h[3];
    uint32_t b_addr = h[6], b_size = h[7], s_addr = h[8], s_size = h[9];
    if (0x800 + (size_t)t_size > len)
        t_size = (uint32_t)(len - 0x800);
    memcpy(ram_ptr(t_addr), data + 0x800, t_size);
    if (b_size)
        memset(ram_ptr(b_addr), 0, b_size);
    c->pc = pc0;
    c->r[28] = gp0;
    if (s_addr) {
        c->r[SP] = s_addr + s_size;
        c->r[30] = c->r[SP];
    }
    LOGI("bios", "loaded exe: pc=%08X t=%08X+%X sp=%08X", pc0, t_addr, t_size,
         c->r[SP]);
    return true;
}

static const char *path_basename(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '\\' || *s == '/' || *s == ':')
            b = s + 1;
    return b;
}

static void strip_version(char *s) {
    char *semi = strchr(s, ';');
    if (semi)
        *semi = 0;
}

void bios_load_exe(CPU *c, const char *path, bool run) {
    char name[128];
    snprintf(name, sizeof name, "%s", path);
    strip_version(name);
    uint32_t lba, size;
    if (!iso_find(name, &lba, &size))
        fatal("LoadExec: %s not found on disc", name);
    uint8_t *buf = malloc(size + 2048);
    iso_read(lba, buf, size);
    if (!load_exe_bytes(c, buf, size))
        fatal("LoadExec: %s is not a PS-X EXE", name);
    free(buf);
    char base[64];
    snprintf(base, sizeof base, "%s", path_basename(name));
    const Module *m = module_find_by_exe(base);
    if (!m)
        fatal("no recompiled module for %s", base);
    module_activate(m);
    if (run) {
        g_stack_top = c->r[SP];
        guest_call(c, c->pc);
        LOGI("bios", "%s returned to BIOS", base);
    }
}

void bios_boot(CPU *c, const char *exe) {
    snprintf(g_pending_exec, sizeof g_pending_exec, "%s", exe);
    /* LoadExec unwinds to here and starts the next executable */
    while (g_pending_exec[0]) {
        char path[128];
        snprintf(path, sizeof path, "%s", g_pending_exec);
        g_pending_exec[0] = 0;
        g_ov_depth = 0; /* LoadExec unwound past whatever was running */
        if (host_setjmp(g_boot_jmp) == 0) {
            c->r[SP] = 0x801FFF00u;
            c->r[30] = c->r[SP];
            bios_load_exe(c, path, true);
            if (g_bios_exit_requested)
                break;
        }
    }
}

static void do_load_exec(CPU *c) {
    char path[128];
    guest_cstr(c->r[A0], path, sizeof path);
    const char *p = path;
    if (!strncasecmp(p, "cdrom:", 6))
        p += 6;
    LOGI("bios", "LoadExec(%s, %08X, %08X)", path, c->r[A1], c->r[A2]);
    snprintf(g_pending_exec, sizeof g_pending_exec, "%s", p);
    /* restore a sane exception state for the new program */
    c->cop0[12] = 0x40000401u;
    g_hook_jmpbuf = 0;
    memset(g_chain, 0, sizeof g_chain);
    memset(g_ev, 0, sizeof g_ev);
    g_istat = 0;
    g_imask = 0;
    host_longjmp(g_boot_jmp);
}

/* ------------------------------------------------------------------ */
/* exception dispatch (interrupts)                                     */

void bios_exception(CPU *c) {
    /* 1. kernel interrupt chains, priority 0 (first) .. 3 */
    for (int prio = 0; prio < 4; prio++) {
        uint32_t e = g_chain[prio];
        int guard = 0;
        while (e && guard++ < 32) {
            uint32_t next = rd32(e);
            uint32_t f2 = rd32(e + 4);
            uint32_t f1 = rd32(e + 8);
            if (f1) {
                guest_call(c, f1);
                uint32_t r = c->r[V0];
                if (r && f2) {
                    c->r[A0] = r;
                    guest_call(c, f2);
                }
            }
            e = next;
        }
    }
    /* 2. HookEntryInt: longjmp into the user handler */
    if (g_hook_jmpbuf) {
        uint32_t b = g_hook_jmpbuf;
        c->r[31] = rd32(b + 0);
        c->r[SP] = rd32(b + 4);
        c->r[30] = rd32(b + 8);
        for (int i = 0; i < 8; i++)
            c->r[16 + i] = rd32(b + 12 + 4u * (uint32_t)i);
        c->r[28] = rd32(b + 44);
        c->r[V0] = 1;
        guest_resume(c, c->r[31]);
        LOGW("bios", "interrupt hook returned without ReturnFromException");
    }
    /* 3. default: ReturnFromException */
}

/* ------------------------------------------------------------------ */
/* memory card file API (bu00:/bu10:)                                  */

typedef struct {
    int used;
    int port;
    int slot;
    uint32_t pos;
    uint32_t size;
    int mode;
} FileDesc;
static FileDesc g_fd[16];

static int parse_card_path(const char *p, int *port, char *name, size_t nsz) {
    /* "bu00:NAME" / "bu10:NAME" */
    if (strncasecmp(p, "bu", 2))
        return 0;
    *port = (p[2] == '1') ? 1 : 0;
    const char *colon = strchr(p, ':');
    if (!colon)
        return 0;
    snprintf(name, nsz, "%s", colon + 1);
    return 1;
}

static uint32_t bios_open(CPU *c) {
    char path[128], name[64];
    int port;
    guest_cstr(c->r[A0], path, sizeof path);
    uint32_t mode = c->r[A1];
    if (!parse_card_path(path, &port, name, sizeof name)) {
        LOGW("bios", "open(%s): unsupported device", path);
        return 0xFFFFFFFFu;
    }
    int slot;
    uint32_t size = 0;
    if (mode & 0x200) { /* FCREAT: size in blocks in upper 16 bits */
        int blocks = (int)(mode >> 16);
        if (blocks <= 0)
            blocks = 1;
        slot = memcard_create(port, name, blocks);
        if (slot < 0)
            return 0xFFFFFFFFu;
    } else {
        slot = memcard_find(port, name, &size);
        if (slot < 0)
            return 0xFFFFFFFFu;
    }
    memcard_file_size(port, slot, &size);
    for (int i = 2; i < 16; i++) {
        if (!g_fd[i].used) {
            g_fd[i] = (FileDesc){1, port, slot, 0, size, (int)mode};
            LOGI("bios", "open(%s, %X) = %d", path, mode, i);
            return (uint32_t)i;
        }
    }
    return 0xFFFFFFFFu;
}

static uint32_t bios_rw(CPU *c, bool write) {
    uint32_t fd = c->r[A0], buf = c->r[A1], len = c->r[A2];
    if (fd >= 16 || !g_fd[fd].used)
        return 0xFFFFFFFFu;
    FileDesc *f = &g_fd[fd];
    if (f->pos + len > f->size)
        len = f->pos < f->size ? f->size - f->pos : 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b;
        if (write) {
            b = (uint8_t)mem_r8(buf + i);
            memcard_file_io(f->port, f->slot, f->pos + i, &b, 1, true);
        } else {
            memcard_file_io(f->port, f->slot, f->pos + i, &b, 1, false);
            mem_w8(buf + i, b);
        }
    }
    f->pos += len;
    if (write)
        memcard_flush(f->port);
    /* the real BIOS signals completion through the card events too */
    defer_event(0xF4000001u, 0x0004, 2000);
    return len;
}

/* DIRENTRY: name[20], attr(4), size(4), next(4), head(4), system[4] */
static int g_find_port, g_find_idx;
static char g_find_pat[64];

bool bios_card_busy(void) {
    if (g_npending)
        return true;
    for (int i = 0; i < 16; i++)
        if (g_fd[i].used)
            return true;
    return false;
}

/* save states: the kernel's events, interrupt chains, heap bounds, the
 * deferred card events and open card files (kernel RAM is in main RAM) */
void bios_state(StateIO *s) {
    state_section(s, "BIOS");
    STATE(s, g_ev);
    STATE(s, g_hook_jmpbuf);
    STATE(s, g_chain);
    STATE(s, g_heap_start);
    STATE(s, g_heap_end);
    STATE(s, g_rand_seed);
    STATE(s, g_pending);
    STATE(s, g_npending);
    STATE(s, g_fd);
    STATE(s, g_find_port);
    STATE(s, g_find_idx);
    STATE(s, g_find_pat);
    STATE(s, g_stack_top);
}

static uint32_t dir_fill(uint32_t dst, int port, int start) {
    char nm[24];
    uint32_t size;
    for (int i = start; i < 15; i++) {
        if (memcard_dir_entry(port, i, nm, &size) &&
            memcard_match(g_find_pat, nm)) {
            for (int k = 0; k < 20; k++)
                mem_w8(dst + (uint32_t)k, k < (int)strlen(nm) ? (uint8_t)nm[k] : 0);
            wr32(dst + 20, 0x50);
            wr32(dst + 24, size);
            wr32(dst + 28, 0);
            wr32(dst + 32, (uint32_t)i + 1);
            g_find_idx = i + 1;
            return dst;
        }
    }
    g_find_idx = 15;
    return 0;
}

static uint32_t bios_firstfile(CPU *c) {
    char path[128], name[64];
    int port;
    guest_cstr(c->r[A0], path, sizeof path);
    if (!parse_card_path(path, &port, name, sizeof name))
        return 0;
    g_find_port = port;
    snprintf(g_find_pat, sizeof g_find_pat, "%s", name);
    defer_event(0xF4000001u, 0x0004, 2000);
    return dir_fill(c->r[A1], port, 0);
}

static uint32_t bios_nextfile(CPU *c) {
    return dir_fill(c->r[A0], g_find_port, g_find_idx);
}

/* ------------------------------------------------------------------ */
/* A0 table                                                            */

static uint32_t memchr_g(uint32_t s, uint32_t ch, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (mem_r8(s + i) == (ch & 0xFF))
            return s + i;
    return 0;
}

int g_bios_trace;

void bios_call_A0(CPU *c) {
    uint32_t fn = c->r[9] & 0xFF;
    if (g_bios_trace) LOGI("bios", "A0:%02X(%08X,%08X,%08X) ra=%08X", fn, c->r[4], c->r[5], c->r[6], c->r[31]);
    uint32_t a0 = c->r[A0], a1 = c->r[A1], a2 = c->r[A2];
    uint32_t r = 0;
    switch (fn) {
    case 0x0E: r = (int32_t)a0 < 0 ? -a0 : a0; break; /* abs */
    case 0x0F: r = (int32_t)a0 < 0 ? -a0 : a0; break; /* labs */
    case 0x10: { /* atoi */
        char s[64];
        guest_cstr(a0, s, sizeof s);
        r = (uint32_t)atoi(s);
        break;
    }
    case 0x11: { /* atol */
        char s[64];
        guest_cstr(a0, s, sizeof s);
        r = (uint32_t)atol(s);
        break;
    }
    case 0x13: /* setjmp(buf) */
        wr32(a0 + 0, c->r[31]);
        wr32(a0 + 4, c->r[SP]);
        wr32(a0 + 8, c->r[30]);
        for (int i = 0; i < 8; i++)
            wr32(a0 + 12 + 4u * (uint32_t)i, c->r[16 + i]);
        wr32(a0 + 44, c->r[28]);
        r = 0;
        break;
    case 0x14: /* longjmp(buf, val) */
        c->r[31] = rd32(a0);
        c->r[SP] = rd32(a0 + 4);
        c->r[30] = rd32(a0 + 8);
        for (int i = 0; i < 8; i++)
            c->r[16 + i] = rd32(a0 + 12 + 4u * (uint32_t)i);
        c->r[28] = rd32(a0 + 44);
        c->r[V0] = a1;
        fatal("longjmp to %08X not supported by the recompiler runtime",
              c->r[31]);
    case 0x15: { /* strcat */
        uint32_t d = a0 + (uint32_t)guest_strlen(a0);
        uint32_t i = 0;
        uint8_t ch;
        do { ch = (uint8_t)mem_r8(a1 + i); mem_w8(d + i, ch); i++; } while (ch);
        r = a0;
        break;
    }
    case 0x16: { /* strncat */
        uint32_t d = a0 + (uint32_t)guest_strlen(a0);
        uint32_t i = 0;
        for (; i < a2; i++) {
            uint8_t ch = (uint8_t)mem_r8(a1 + i);
            if (!ch) break;
            mem_w8(d + i, ch);
        }
        mem_w8(d + i, 0);
        r = a0;
        break;
    }
    case 0x17: { /* strcmp */
        int32_t res = 0;
        for (uint32_t i = 0;; i++) {
            uint8_t x = (uint8_t)mem_r8(a0 + i), y = (uint8_t)mem_r8(a1 + i);
            if (x != y) { res = x - y; break; }
            if (!x) break;
        }
        r = (uint32_t)res;
        break;
    }
    case 0x18: { /* strncmp */
        int32_t res = 0;
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t x = (uint8_t)mem_r8(a0 + i), y = (uint8_t)mem_r8(a1 + i);
            if (x != y) { res = x - y; break; }
            if (!x) break;
        }
        r = (uint32_t)res;
        break;
    }
    case 0x19: { /* strcpy */
        uint32_t i = 0;
        uint8_t ch;
        do { ch = (uint8_t)mem_r8(a1 + i); mem_w8(a0 + i, ch); i++; } while (ch);
        r = a0;
        break;
    }
    case 0x1A: { /* strncpy */
        uint32_t i = 0;
        for (; i < a2; i++) {
            uint8_t ch = (uint8_t)mem_r8(a1 + i);
            mem_w8(a0 + i, ch);
            if (!ch) break;
        }
        for (; i < a2; i++)
            mem_w8(a0 + i, 0);
        r = a0;
        break;
    }
    case 0x1B: r = (uint32_t)guest_strlen(a0); break; /* strlen */
    case 0x1C: /* index */
    case 0x1E: /* strchr */
        r = 0;
        for (uint32_t i = 0;; i++) {
            uint8_t ch = (uint8_t)mem_r8(a0 + i);
            if (ch == (a1 & 0xFF)) { r = a0 + i; break; }
            if (!ch) break;
        }
        break;
    case 0x1D: /* rindex */
    case 0x1F: /* strrchr */
        r = 0;
        for (uint32_t i = 0;; i++) {
            uint8_t ch = (uint8_t)mem_r8(a0 + i);
            if (ch == (a1 & 0xFF)) r = a0 + i;
            if (!ch) break;
        }
        break;
    case 0x24: { /* strstr */
        size_t nl = guest_strlen(a1);
        r = 0;
        for (uint32_t i = 0; mem_r8(a0 + i); i++) {
            size_t k = 0;
            while (k < nl && mem_r8(a0 + i + k) == mem_r8(a1 + k))
                k++;
            if (k == nl) { r = a0 + i; break; }
        }
        if (nl == 0) r = a0;
        break;
    }
    case 0x25: r = (uint32_t)toupper((int)(a0 & 0xFF)); break;
    case 0x26: r = (uint32_t)tolower((int)(a0 & 0xFF)); break;
    case 0x28: /* bzero */
        for (uint32_t i = 0; i < a1; i++) mem_w8(a0 + i, 0);
        r = a0;
        break;
    case 0x2A: /* memcpy */
        for (uint32_t i = 0; i < a2; i++) mem_w8(a0 + i, mem_r8(a1 + i));
        r = a0;
        break;
    case 0x2B: /* memset */
        for (uint32_t i = 0; i < a2; i++) mem_w8(a0 + i, a1);
        r = a0;
        break;
    case 0x2C: /* memmove */
        if (a0 < a1) {
            for (uint32_t i = 0; i < a2; i++) mem_w8(a0 + i, mem_r8(a1 + i));
        } else {
            for (uint32_t i = a2; i-- > 0;) mem_w8(a0 + i, mem_r8(a1 + i));
        }
        r = a0;
        break;
    case 0x2D: { /* memcmp */
        int32_t res = 0;
        for (uint32_t i = 0; i < a2; i++) {
            int d = (int)mem_r8(a0 + i) - (int)mem_r8(a1 + i);
            if (d) { res = d; break; }
        }
        r = (uint32_t)res;
        break;
    }
    case 0x2E: r = memchr_g(a0, a1, a2); break; /* memchr */
    case 0x2F: /* rand */
        g_rand_seed = g_rand_seed * 1103515245u + 12345u;
        r = (g_rand_seed >> 16) & 0x7FFF;
        break;
    case 0x30: g_rand_seed = a0; break; /* srand */
    case 0x33: r = heap_alloc(a0); break; /* malloc */
    case 0x34: heap_free(a0); break;      /* free */
    case 0x37: { /* calloc */
        r = heap_alloc(a0 * a1);
        for (uint32_t i = 0; r && i < a0 * a1; i++) mem_w8(r + i, 0);
        break;
    }
    case 0x39: heap_init(a0, a1); break; /* InitHeap */
    case 0x3B: /* getchar */
        r = 0xFFFFFFFFu;
        break;
    case 0x3C: { /* putchar */
        char s[2] = {(char)a0, 0};
        guest_print(s);
        break;
    }
    case 0x3E: { /* puts */
        char s[512];
        guest_cstr(a0, s, sizeof s);
        guest_print(s);
        guest_print("\n");
        break;
    }
    case 0x3F: { /* printf */
        char s[1024];
        r = (uint32_t)guest_format(c, a0, 1, s, sizeof s);
        guest_print(s);
        break;
    }
    case 0x44: /* FlushCache */
        break;
    case 0x49: /* GPU_cw */
        gpu_write_gp0(a0);
        break;
    case 0x51: /* LoadExec */
        do_load_exec(c);
        break;
    case 0x70: /* _bu_init */
        memcard_init_all();
        break;
    case 0x71: /* _96_init (CdInit) */
    case 0x72: /* _96_remove */
        break;
    case 0x9F: /* SetMem */
        break;
    case 0xA1: { /* SystemError */
        fatal("SystemError(%c, %u)", (char)a0, a1);
    }
    case 0xAB: /* _card_info(port) */
        defer_event(0xF4000001u, memcard_present((int)((a0 >> 4) & 1)) ? 0x0004 : 0x0100,
                    20000);
        r = 1;
        break;
    case 0xAC: /* _card_load(port) */
        defer_event(0xF4000001u, 0x0004, 20000);
        r = 1;
        break;
    default:
        LOGW("bios", "unimplemented A0:%02X (a0=%08X a1=%08X) ra=%08X", fn,
             a0, a1, c->r[31]);
        break;
    }
    c->r[V0] = r;
}

/* ------------------------------------------------------------------ */
/* B0 table                                                            */

void bios_call_B0(CPU *c) {
    uint32_t fn = c->r[9] & 0xFF;
    if (g_bios_trace && fn != 0x17 && fn != 0x0B) LOGI("bios", "B0:%02X(%08X,%08X,%08X) ra=%08X", fn, c->r[4], c->r[5], c->r[6], c->r[31]);
    uint32_t a0 = c->r[A0], a1 = c->r[A1], a2 = c->r[A2], a3 = c->r[A3];
    uint32_t r = 0;
    switch (fn) {
    case 0x00: r = heap_alloc(a0); break; /* alloc_kernel_memory */
    case 0x01: heap_free(a0); break;
    case 0x07: deliver_event(c, a0, a1); break; /* DeliverEvent */
    case 0x08: r = open_event(a0, a1, a2, a3); break;
    case 0x09: { /* CloseEvent */
        EvCB *e = ev_get(a0);
        if (e) e->status = 0;
        r = 1;
        break;
    }
    case 0x0A: { /* WaitEvent */
        EvCB *e = ev_get(a0);
        if (!e || e->status == 0x1000) { r = 0; break; }
        int64_t limit = now_cycles() + 10LL * PSX_CPU_HZ;
        while (e->status != 0x4000 && now_cycles() < limit) {
            c->cycles += 1000;
            cpu_service_events(c);
        }
        if (e->status == 0x4000) e->status = 0x2000;
        r = 1;
        break;
    }
    case 0x0B: { /* TestEvent */
        EvCB *e = ev_get(a0);
        if (e && e->status == 0x4000) {
            e->status = 0x2000;
            r = 1;
        }
        break;
    }
    case 0x0C: { /* EnableEvent */
        EvCB *e = ev_get(a0);
        if (e && e->status != 0) e->status = 0x2000;
        r = 1;
        break;
    }
    case 0x0D: { /* DisableEvent */
        EvCB *e = ev_get(a0);
        if (e && e->status != 0) e->status = 0x1000;
        r = 1;
        break;
    }
    case 0x17: /* ReturnFromException */
        LOGD("bios", "ReturnFromException");
        cpu_return_from_exception(c);
    case 0x18: /* ResetEntryInt */
        LOGD("bios", "ResetEntryInt()");
        g_hook_jmpbuf = 0;
        break;
    case 0x19: /* HookEntryInt */
        LOGD("bios", "HookEntryInt(%08X) ra=%08X", a0, rd32(a0));
        g_hook_jmpbuf = a0;
        break;
    case 0x20: /* UnDeliverEvent */
        for (int i = 0; i < MAX_EVENTS; i++)
            if (g_ev[i].status == 0x4000 && g_ev[i].cls == a0 &&
                g_ev[i].spec == a1 && g_ev[i].mode == 0x2000)
                g_ev[i].status = 0x2000;
        break;
    case 0x32: r = bios_open(c); break;
    case 0x33: { /* lseek(fd, off, whence) */
        if (a0 < 16 && g_fd[a0].used) {
            if (a2 == 0) g_fd[a0].pos = a1;
            else if (a2 == 1) g_fd[a0].pos += a1;
            r = g_fd[a0].pos;
        } else {
            r = 0xFFFFFFFFu;
        }
        break;
    }
    case 0x34: r = bios_rw(c, false); break; /* read */
    case 0x35: /* write */
        if (a0 == 1) { /* stdout */
            char s[512];
            uint32_t n = a2 < 511 ? a2 : 511;
            for (uint32_t i = 0; i < n; i++) s[i] = (char)mem_r8(a1 + i);
            s[n] = 0;
            guest_print(s);
            r = a2;
        } else {
            r = bios_rw(c, true);
        }
        break;
    case 0x36: /* close */
        if (a0 < 16) g_fd[a0].used = 0;
        r = a0;
        break;
    case 0x38: /* exit */
        LOGI("bios", "exit(%d)", (int)a0);
        g_bios_exit_requested = true;
        g_pending_exec[0] = 0;
        host_longjmp(g_boot_jmp);
    case 0x3D: { /* putchar */
        char s[2] = {(char)a0, 0};
        guest_print(s);
        break;
    }
    case 0x3F: { /* puts */
        char s[512];
        guest_cstr(a0, s, sizeof s);
        guest_print(s);
        guest_print("\n");
        break;
    }
    case 0x41: { /* format(path) */
        char path[64], name[32];
        int port;
        guest_cstr(a0, path, sizeof path);
        r = parse_card_path(path, &port, name, sizeof name) &&
            memcard_format(port);
        break;
    }
    case 0x42: r = bios_firstfile(c); break;
    case 0x43: r = bios_nextfile(c); break;
    case 0x44: { /* rename */
        char p1[64], p2[64], n1[32], n2[32];
        int port1, port2;
        guest_cstr(a0, p1, sizeof p1);
        guest_cstr(a1, p2, sizeof p2);
        r = parse_card_path(p1, &port1, n1, sizeof n1) &&
            parse_card_path(p2, &port2, n2, sizeof n2) &&
            memcard_rename(port1, n1, n2);
        break;
    }
    case 0x45: { /* delete */
        char path[64], name[32];
        int port;
        guest_cstr(a0, path, sizeof path);
        r = parse_card_path(path, &port, name, sizeof name) &&
            memcard_delete(port, name);
        defer_event(0xF4000001u, 0x0004, 2000);
        break;
    }
    case 0x4A: /* InitCARD */
    case 0x4B: /* StartCARD */
    case 0x4C: /* StopCARD */
        r = 1;
        break;
    case 0x4D: /* _card_info (old) */
        defer_event(0xF4000001u, 0x0004, 20000);
        r = 1;
        break;
    case 0x4E: /* _card_write(port, sector, buf) */
    case 0x4F: { /* _card_read(port, sector, buf) */
        int port = (int)((a0 >> 4) & 1);
        uint8_t sec[128];
        if (fn == 0x4E) {
            for (int i = 0; i < 128; i++) sec[i] = (uint8_t)mem_r8(a2 + (uint32_t)i);
            memcard_sector_io(port, (int)a1, sec, true);
        } else {
            memcard_sector_io(port, (int)a1, sec, false);
            for (int i = 0; i < 128; i++) mem_w8(a2 + (uint32_t)i, sec[i]);
        }
        /* sector I/O completion is reported through HwCARD events */
        defer_event(0xF0000011u, memcard_present(port) ? 0x0004 : 0x0100, 20000);
        r = 1;
        break;
    }
    case 0x50: /* _new_card */
        break;
    case 0x56: r = KERNEL_C0_TABLE; break; /* GetC0Table */
    case 0x57: r = KERNEL_B0_TABLE; break; /* GetB0Table */
    case 0x5B: /* ChangeClearPAD */
        break;
    case 0x5C: /* _card_status */
        r = 0x11;
        break;
    case 0x5D: /* _card_wait */
        r = 1;
        break;
    default:
        LOGW("bios", "unimplemented B0:%02X (a0=%08X a1=%08X) ra=%08X", fn,
             a0, a1, c->r[31]);
        break;
    }
    c->r[V0] = r;
}

/* ------------------------------------------------------------------ */
/* C0 table                                                            */

void bios_call_C0(CPU *c) {
    uint32_t fn = c->r[9] & 0xFF;
    if (g_bios_trace) LOGI("bios", "C0:%02X(%08X,%08X,%08X) ra=%08X", fn, c->r[4], c->r[5], c->r[6], c->r[31]);
    uint32_t a0 = c->r[A0], a1 = c->r[A1];
    uint32_t r = 0;
    switch (fn) {
    case 0x02: /* SysEnqIntRP(prio, struct) */
        LOGD("bios", "SysEnqIntRP(%u, %08X) f1=%08X f2=%08X", a0, a1, rd32(a1 + 8), rd32(a1 + 4));
        if (a0 < 4) {
            wr32(a1, g_chain[a0]);
            g_chain[a0] = a1;
        }
        r = 0;
        break;
    case 0x03: /* SysDeqIntRP(prio, struct) */
        if (a0 < 4) {
            uint32_t *pp = &g_chain[a0];
            uint32_t e = *pp;
            uint32_t prev = 0;
            while (e) {
                if (e == a1) {
                    uint32_t nx = rd32(e);
                    if (prev) wr32(prev, nx); else *pp = nx;
                    break;
                }
                prev = e;
                e = rd32(e);
            }
        }
        r = 0;
        break;
    case 0x0A: /* ChangeClearRCnt(t, flag) */
        r = 1;
        break;
    default:
        LOGW("bios", "unimplemented C0:%02X (a0=%08X a1=%08X) ra=%08X", fn,
             a0, a1, c->r[31]);
        break;
    }
    c->r[V0] = r;
}

/* ------------------------------------------------------------------ */

void bios_init(CPU *c) {
    memset(g_ram, 0, sizeof g_ram);
    memset(g_ev, 0, sizeof g_ev);
    memset(g_fd, 0, sizeof g_fd);
    g_hook_jmpbuf = 0;
    memset(g_chain, 0, sizeof g_chain);
    /* Kernel tables: each entry points at a harmless dummy area so code
     * that patches kernel routines writes into unused low RAM. */
    for (uint32_t i = 0; i < 0xC0; i++) {
        wr32(KERNEL_A0_TABLE + 4 * i, KERNEL_DUMMY);
        wr32(KERNEL_B0_TABLE + 4 * i, KERNEL_DUMMY);
    }
    for (uint32_t i = 0; i < 0x20; i++)
        wr32(KERNEL_C0_TABLE + 4 * i, KERNEL_DUMMY);
    /* devices "cdrom" and "bu" (memory card); handlers are never called
     * (file I/O is HLE), they only need to look valid */
    static const struct { const char *name; uint32_t flags, ssize; } dev[] = {
        {"cdrom", 0x14, 0x800}, {"bu", 0x14, 0x80},
    };
    uint32_t str = KERNEL_DCB_NAMES;
    for (uint32_t i = 0; i < sizeof dev / sizeof dev[0]; i++) {
        uint32_t e = KERNEL_DCB_TABLE + i * DCB_ENTRY;
        wr32(e + 0x00, str);
        wr32(e + 0x04, dev[i].flags);
        wr32(e + 0x08, dev[i].ssize);
        wr32(e + 0x0C, str);
        for (uint32_t off = 0x10; off < DCB_ENTRY; off += 4)
            wr32(e + off, KERNEL_DUMMY);
        for (const char *n = dev[i].name; ; n++, str++) {
            mem_w8(str, (uint8_t)*n);
            if (!*n)
                break;
        }
        str = (str + 4) & ~3u;
    }
    wr32(KERNEL_DCB_PTR, KERNEL_DCB_TABLE);
    wr32(KERNEL_DCB_SIZE, DCB_MAX * DCB_ENTRY);
    c->cop0[12] = 0x40000401u; /* CU2 | IM2 | IEc */
    c->cop0[15] = 2;
    sched_register(EV_BIOS, bios_deferred_tick);
}
