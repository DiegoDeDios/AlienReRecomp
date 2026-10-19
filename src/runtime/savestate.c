/*
 * Save states: the whole emulated machine (CPU, RAM, every hardware
 * register, VRAM, SPU RAM, the CD controller and the BIOS bookkeeping)
 * written to a file and read back. Memory cards are not part of a state:
 * the game's own saves stay as they are.
 *
 * A state is taken and restored at one kind of safe point only: entry to
 * libetc's VSync, which the menus wait in and gameplay polls every frame,
 * on the main executable's main thread (not in an interrupt handler). The
 * host must be in the same place too, which depends on how the game's code
 * runs:
 *  - recompiled builds: every MIPS function on the call chain is a C
 *    function on the host stack, so a state is restored only where the
 *    game's call chain matches the one it was taken in, compared through
 *    the return addresses on the game's stack (in practice: in game, not
 *    from the menus);
 *  - interpreted builds (the releases) run guest calls in one loop and only
 *    the native overrides being run add host frames, so a state can be
 *    restored at any VSync call with the same overrides active (menus
 *    included); the interpreter then continues at the restored return
 *    address.
 */
#include <stdlib.h>

#include "../platform/launcher.h" /* PORT_VERSION */
#include "pgxp.h"
#include "psx.h"
#include "savestate.h"

#define STATE_MAGIC "AR-STATE"
#define STATE_FORMAT 1
#define MAX_FRAMES 128

/* ------------------------------------------------------------------ */
/* serialization helpers                                               */

void state_io(StateIO *s, void *p, size_t n) {
    if (s->error)
        return;
    if (s->loading) {
        if (n > s->size - s->pos) {
            s->error = true;
            return;
        }
        memcpy(p, s->buf + s->pos, n);
    } else {
        if (n > s->cap - s->pos) {
            size_t cap = s->cap ? s->cap : (size_t)8 << 20;
            while (cap - s->pos < n)
                cap *= 2;
            uint8_t *b = realloc(s->buf, cap);
            if (!b) {
                s->error = true;
                return;
            }
            s->buf = b;
            s->cap = cap;
        }
        memcpy(s->buf + s->pos, p, n);
        s->size = s->pos + n;
    }
    s->pos += n;
}

void state_section(StateIO *s, const char *tag4) {
    char t[4];
    memcpy(t, tag4, 4);
    state_io(s, t, 4);
    if (s->loading && !s->error && memcmp(t, tag4, 4)) {
        LOGW("state", "expected section %.4s, found %.4s", tag4, t);
        s->error = true;
    }
}

void state_heap(StateIO *s, void **p, size_t *len, size_t *cap, size_t elem) {
    uint64_t n = *len;
    STATE(s, n);
    if (s->error)
        return;
    if (s->loading) {
        if (n > ((uint64_t)64 << 20) / elem) {
            s->error = true;
            return;
        }
        if (n > *cap) {
            void *b = realloc(*p, (size_t)n * elem);
            if (!b) {
                s->error = true;
                return;
            }
            *p = b;
            *cap = (size_t)n;
        }
        *len = (size_t)n;
    }
    if (n)
        state_io(s, *p, (size_t)n * elem);
}

static void serialize(StateIO *s) {
    cpu_state(s);
    bus_state(s);
    bios_state(s);
    interp_state(s);
    dma_state(s);
    timers_state(s);
    gpu_state(s);
    cdrom_state(s);
    spu_state(s);
    mdec_state(s);
    sio_state(s);
    ws_state(s);
    platform_state(s);
    state_section(s, "END ");
}

/* ------------------------------------------------------------------ */
/* file header                                                         */

typedef struct SafePoint {
    char module[16];
    uint32_t ra, sp;
    uint32_t n;
    /* recompiled: {stack address, return address} of every frame on the
     * game's stack; interpreted: {override address, 0} innermost last */
    uint32_t frames[MAX_FRAMES][2];
} SafePoint;

typedef struct Header {
    char magic[8];
    uint32_t format;
    char version[16];
    uint32_t interpreted;
    uint32_t disc;
    uint32_t payload_crc;
    uint64_t payload_size;
    SafePoint at;
} Header;

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* which disc the state belongs to: its volume descriptor and size */
static uint32_t disc_signature(void) {
    uint8_t raw[2352];
    if (!disc_read_sector(16, raw))
        return 0;
    return crc32_buf(disc_total_sectors(), raw, sizeof raw);
}

/* ------------------------------------------------------------------ */
/* the safe point                                                      */

static bool is_return_address(uint32_t v) {
    if ((v & 3) || v < 0x80010000u || v >= 0x80200000u)
        return false;
    uint32_t w = mem_r32(v - 8);
    uint32_t op = w >> 26;
    return op == 3 ||                                  /* jal */
           (op == 0 && (w & 0x3F) == 9) ||             /* jalr */
           (op == 1 && ((w >> 16) & 0x1E) == 0x10);    /* bltzal / bgezal */
}

static bool capture_point(CPU *c, SafePoint *p, const char **why) {
    memset(p, 0, sizeof *p);
    const Module *m = module_current();
    if (!m || strcmp(m->name, "ALIEN.BIN")) {
        *why = "not available during the intro";
        return false;
    }
    if (cpu_exception_depth()) {
        *why = "the game is busy, try again";
        return false;
    }
    snprintf(p->module, sizeof p->module, "%s", m->name);
    p->ra = c->r[31];
    p->sp = c->r[29];
    if (g_interp_only) {
        if (g_ov_depth > (int)(sizeof g_ov_chain / sizeof g_ov_chain[0])) {
            *why = "the game is busy, try again";
            return false;
        }
        for (int i = 0; i < g_ov_depth; i++)
            p->frames[p->n++][0] = g_ov_chain[i];
        return true;
    }
    uint32_t top = bios_stack_top();
    for (uint32_t a = p->sp; a < top && p->n < MAX_FRAMES; a += 4) {
        uint32_t v = mem_r32(a);
        if (is_return_address(v)) {
            p->frames[p->n][0] = a;
            p->frames[p->n][1] = v;
            p->n++;
        }
    }
    return true;
}

/* Interpreted builds resume the caller at the restored return address, so
 * only the overrides being run must match; recompiled builds need the very
 * same call chain. */
static bool same_point(const SafePoint *a, const SafePoint *b) {
    if (strcmp(a->module, b->module) || a->n != b->n ||
        memcmp(a->frames, b->frames, a->n * sizeof a->frames[0]))
        return false;
    return g_interp_only || (a->ra == b->ra && a->sp == b->sp);
}

/* ------------------------------------------------------------------ */
/* save and load                                                       */

/* A request waits for a usable safe point: not inside an interrupt
 * handler, no memory card access in progress and, for a load, the right
 * call chain. The frontend gives up after a few seconds (state_tick). */
#define REQUEST_TIMEOUT_FRAMES 180

static int g_req; /* 0 none, 1 save, 2 load */
static char g_req_path[1100];
static int g_req_frames;
static uint8_t *g_load_buf; /* the file being loaded, read on the first try */
static size_t g_load_size;
unsigned g_state_loads;

static void request(int kind, const char *path) {
    snprintf(g_req_path, sizeof g_req_path, "%s", path);
    free(g_load_buf);
    g_load_buf = NULL;
    g_req_frames = 0;
    g_req = kind;
}

void state_request_save(const char *path) { request(1, path); }
void state_request_load(const char *path) { request(2, path); }
bool state_request_pending(void) { return g_req != 0; }

static void finish(bool ok, const char *message) {
    bool save = g_req == 1;
    g_req = 0;
    free(g_load_buf);
    g_load_buf = NULL;
    platform_state_done(save, ok, message);
}

void state_tick(void) {
    if (g_req && ++g_req_frames > REQUEST_TIMEOUT_FRAMES) {
        LOGW("state", "%s %s: no safe point reached", g_req == 1 ? "save" : "load", g_req_path);
        finish(false, g_req == 1 ? "not available right now"
                                 : (g_interp_only ? "not available right now"
                                                  : "load it in game, not in the menus"));
    }
}

static bool write_file(const char *path, const Header *h, const uint8_t *data, size_t n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(h, sizeof *h, 1, f) == 1 && fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok && rename(tmp, path)) {
        /* Windows: rename does not replace an existing file */
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok)
        remove(tmp);
    return ok;
}

/* returns false to try again at the next safe point */
static bool try_save(CPU *c) {
    Header h;
    memset(&h, 0, sizeof h);
    const char *why = NULL;
    if (!capture_point(c, &h.at, &why) || bios_card_busy())
        return false;
    StateIO s = {0};
    serialize(&s);
    if (s.error) {
        free(s.buf);
        finish(false, "out of memory");
        return true;
    }
    memcpy(h.magic, STATE_MAGIC, 8);
    h.format = STATE_FORMAT;
    snprintf(h.version, sizeof h.version, "%s", PORT_VERSION);
    h.interpreted = g_interp_only;
    h.disc = disc_signature();
    h.payload_size = s.size;
    h.payload_crc = crc32_buf(0, s.buf, s.size);
    bool ok = write_file(g_req_path, &h, s.buf, s.size);
    free(s.buf);
    if (!ok) {
        LOGW("state", "cannot write %s", g_req_path);
        finish(false, "cannot write the file");
        return true;
    }
    LOGI("state", "saved %s (%u KiB)", g_req_path, (unsigned)((sizeof h + h.payload_size) >> 10));
    finish(true, NULL);
    return true;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        long len = ftell(f);
        if (len > 0 && len < (256L << 20) && !fseek(f, 0, SEEK_SET)) {
            buf = malloc((size_t)len);
            if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
                free(buf);
                buf = NULL;
            }
            *n = (size_t)len;
        }
    }
    fclose(f);
    return buf;
}

/* why a file cannot be loaded at all, or NULL */
static const char *check_header(const Header *h, size_t file_size) {
    static char msg[96];
    if (file_size < sizeof *h || memcmp(h->magic, STATE_MAGIC, 8))
        return "not a quick save file";
    if (h->format != STATE_FORMAT || strncmp(h->version, PORT_VERSION, sizeof h->version)) {
        snprintf(msg, sizeof msg, "made with version %.15s of the port", h->version);
        return msg;
    }
    if (h->interpreted != (uint32_t)g_interp_only)
        return g_interp_only ? "made with a developer build" : "made with a release build";
    if (h->disc != disc_signature())
        return "made with a different disc";
    if (h->payload_size != file_size - sizeof *h ||
        crc32_buf(0, (const uint8_t *)(h + 1), (size_t)h->payload_size) != h->payload_crc)
        return "the file is damaged";
    return NULL;
}

static bool try_load(CPU *c) {
    if (!g_load_buf) {
        g_load_buf = read_file(g_req_path, &g_load_size);
        if (!g_load_buf) {
            finish(false, "no quick save yet");
            return true;
        }
        const char *why = check_header((const Header *)g_load_buf, g_load_size);
        if (why) {
            LOGW("state", "cannot load %s: %s", g_req_path, why);
            finish(false, why);
            return true;
        }
    }
    const Header *h = (const Header *)g_load_buf;
    SafePoint here;
    const char *why = NULL;
    if (!capture_point(c, &here, &why) || !same_point(&h->at, &here))
        return false;
    /* keep the current state to fall back on if the file cannot be read */
    StateIO backup = {0};
    serialize(&backup);
    StateIO in = {g_load_buf + sizeof *h, 0, (size_t)h->payload_size, (size_t)h->payload_size,
                  true, false};
    serialize(&in);
    bool ok = !in.error && in.pos == in.size && !backup.error;
    if (!ok && !backup.error) {
        StateIO back = {backup.buf, 0, backup.size, backup.cap, true, false};
        serialize(&back);
    }
    free(backup.buf);
    if (!ok) {
        LOGE("state", "cannot load %s: unexpected contents", g_req_path);
        finish(false, "the file is damaged");
        return true;
    }
    c->next_event = sched_next();
    cpu_request_check();
    pgxp_reset();
    g_state_loads++;
    platform_state_loaded();
    LOGI("state", "loaded %s", g_req_path);
    finish(true, NULL);
    return true;
}

void state_safe_point(CPU *c) {
    if (g_req == 1)
        try_save(c);
    else if (g_req == 2)
        try_load(c);
}
