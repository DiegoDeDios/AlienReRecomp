/*
 * Native runtime: hardware model and HLE BIOS interfaces.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host non-local jumps (interrupt return, LoadExec). On 64-bit Windows the
 * C library's longjmp unwinds the stack frame by frame (SEH); GCC's
 * builtins only restore the registers, like longjmp on other systems. */
#if defined(_WIN32) && defined(__GNUC__)
typedef void *host_jmp_buf[5];
#define host_setjmp(b) __builtin_setjmp(b)
#define host_longjmp(b) __builtin_longjmp((b), 1)
#else
#include <setjmp.h>
typedef jmp_buf host_jmp_buf;
#define host_setjmp(b) setjmp(b)
#define host_longjmp(b) longjmp((b), 1)
#endif

#define PSX_CPU_HZ 33868800
#define PSX_NTSC_FRAME_CYCLES 564480 /* 33868800 / 60 */
#define PSX_NTSC_LINES 263
#define PSX_HBLANK_CYCLES (PSX_NTSC_FRAME_CYCLES / PSX_NTSC_LINES)

extern CPU *g_cpu;

/* ---- logging ---- */
/* MinGW builds use MinGW's C99 printf (__USE_MINGW_ANSI_STDIO), so formats
 * are checked as GNU printf (%zu etc.) rather than Microsoft's */
#ifdef __MINGW32__
#define PSX_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#else
#define PSX_PRINTF(f, a) __attribute__((format(printf, f, a)))
#endif

enum { LOG_ERR, LOG_WARN, LOG_INFO, LOG_DBG };
extern int g_log_level;
void log_msg(int level, const char *sys, const char *fmt, ...) PSX_PRINTF(3, 4);
#define LOGE(sys, ...) log_msg(LOG_ERR, sys, __VA_ARGS__)
#define LOGW(sys, ...) log_msg(LOG_WARN, sys, __VA_ARGS__)
#define LOGI(sys, ...) log_msg(LOG_INFO, sys, __VA_ARGS__)
#define LOGD(sys, ...) do { if (g_log_level >= LOG_DBG) log_msg(LOG_DBG, sys, __VA_ARGS__); } while (0)
_Noreturn void fatal(const char *fmt, ...) PSX_PRINTF(1, 2);

/* ---- modules (recompiled executables) ---- */
typedef struct Module {
    const char *name;     /* e.g. "ALIEN.BIN" */
    const FuncEntry *funcs;
    unsigned nfuncs;
    const ResumeEntry *resume;
    unsigned nresume;
} Module;
const Module *module_find_by_exe(const char *name);
void module_activate(const Module *m);
const Module *module_current(void);
RecompFunc module_lookup(uint32_t addr);
const char *module_func_name(uint32_t addr);

/* Run guest code at addr as a call (sets ra to a sentinel, returns when the
 * callee returns). */
void guest_call(CPU *c, uint32_t addr);
/* Resume execution at an arbitrary address (setjmp resume or function). */
void guest_resume(CPU *c, uint32_t addr);

/* ---- scheduler ---- */
typedef enum {
    EV_VBLANK,
    EV_HBLANK,
    EV_TIMER0,
    EV_TIMER1,
    EV_TIMER2,
    EV_CDROM,
    EV_CDROM_READ,
    EV_SPU,
    EV_DMA,
    EV_SIO,
    EV_MDEC,
    EV_GPU,
    EV_BIOS,
    EV_COUNT
} EventId;
typedef void (*EventFn)(int64_t now, int64_t late);
void sched_init(void);
void sched_register(EventId id, EventFn fn);
void sched_at(EventId id, int64_t when);
void sched_in(EventId id, int64_t delta);
void sched_cancel(EventId id);
bool sched_pending(EventId id);
int64_t sched_time(EventId id);
void sched_run(int64_t now);
int64_t sched_next(void);
static inline int64_t now_cycles(void) { return g_cpu->cycles; }
void cpu_request_check(void); /* force event/IRQ check at the next point */

/* ---- interrupts ---- */
enum {
    IRQ_VBLANK = 0,
    IRQ_GPU = 1,
    IRQ_CDROM = 2,
    IRQ_DMA = 3,
    IRQ_TIMER0 = 4,
    IRQ_TIMER1 = 5,
    IRQ_TIMER2 = 6,
    IRQ_SIO0 = 7,
    IRQ_SIO = 8,
    IRQ_SPU = 9,
    IRQ_PIO = 10,
};
extern uint32_t g_istat, g_imask;
void irq_raise(int n);
void cpu_take_interrupt(CPU *c);
void cpu_return_from_exception(CPU *c) __attribute__((noreturn));

/* ---- HLE BIOS ---- */
void bios_init(CPU *c);
void bios_exception(CPU *c);
void bios_load_exe(CPU *c, const char *path, bool run);
void bios_boot(CPU *c, const char *exe);
extern bool g_bios_exit_requested;

/* ---- DMA ---- */
void dma_init(void);
uint32_t dma_read(uint32_t addr);
void dma_write(uint32_t addr, uint32_t v);
void dma_write_sized(uint32_t addr, uint32_t v, int size);
void dma_irq_complete(int ch);

/* ---- timers ---- */
void timers_init(void);
uint32_t timers_read(uint32_t addr);
void timers_write(uint32_t addr, uint32_t v);
void timers_hblank(void);
void timers_vblank(bool start);

/* ---- GPU ---- */
void gpu_init(void);
uint32_t gpu_read_data(void);
uint32_t gpu_read_stat(void);
void gpu_write_gp0(uint32_t v);
void gpu_write_gp1(uint32_t v);
void gpu_dma_write(uint32_t addr, uint32_t words); /* block mode */
void gpu_dma_read(uint32_t addr, uint32_t words);
uint32_t gpu_dma_linked_list(uint32_t addr);   /* returns words processed */
void gpu_vblank(void);
uint16_t *gpu_vram(void);
typedef struct DisplayInfo {
    int x, y;           /* top-left in VRAM */
    int w, h;           /* active display size in VRAM pixels */
    bool rgb24;
    bool enabled;
    bool interlaced;
    bool pal;
    int hres;           /* nominal horizontal resolution */
} DisplayInfo;
void gpu_display_info(DisplayInfo *d);
extern uint64_t g_gpu_frame_counter;
extern uint64_t g_gpu_flip_counter;

/* ---- CD-ROM ---- */
bool disc_open(const char *cue_path);
void disc_close(void);
bool disc_read_sector(uint32_t lba, uint8_t *raw2352);
uint32_t disc_track_count(void);
uint32_t disc_track_start(int track); /* LBA (absolute, incl. 2s pregap) */
uint32_t disc_total_sectors(void);
bool disc_track_is_audio(int track);
void cdrom_init(void);
uint32_t cdrom_read(uint32_t addr);
void cdrom_write(uint32_t addr, uint32_t v);
void cdrom_dma_read(uint32_t addr, uint32_t words);
/* audio from CD (XA / CD-DA) into the SPU mixer: stereo 44.1 kHz */
int cdrom_audio_pull(int16_t *out, int frames);

/* ---- SPU ---- */
void spu_init(void);
uint32_t spu_read16(uint32_t addr);
void spu_write16(uint32_t addr, uint32_t v);
void spu_dma_write(uint32_t addr, uint32_t words);
void spu_dma_read(uint32_t addr, uint32_t words);
void spu_generate(int16_t *out, int frames);

/* ---- MDEC ---- */
void mdec_init(void);
uint32_t mdec_read(uint32_t addr);
void mdec_write(uint32_t addr, uint32_t v);
void mdec_dma_in(uint32_t addr, uint32_t words);
void mdec_dma_out(uint32_t addr, uint32_t words);

/* ---- SIO0 (pads / memory cards) ---- */
void sio_init(void);
uint32_t sio_read(uint32_t addr, int size);
void sio_write(uint32_t addr, uint32_t v, int size);
/* pad state: PS1 button bits, active-low handled internally (1=pressed) */
enum {
    PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000,
    PAD_SQUARE = 0x8000,
};
void pad_set_buttons(int port, uint16_t pressed);
void pad_set_analog(int port, uint8_t rx, uint8_t ry, uint8_t lx, uint8_t ly);
/* the sticks as last set by the frontend (0x80 = centre) */
void pad_get_sticks(int port, uint8_t *rx, uint8_t *ry, uint8_t *lx, uint8_t *ly);

/* ---- game enhancements (native.c) ---- */
extern bool g_sprint;      /* sprint input held this frame */
extern int g_sprint_pct;   /* player movement speed while sprinting, percent */
extern bool g_infinite_flashlight; /* flashlight battery never runs down */

/* ---- platform (frontend) ---- */
void platform_vblank(void);   /* present + input + pacing, called each vblank */
bool platform_should_quit(void);
const char *platform_data_dir(void);

#ifdef __cplusplus
}
#endif
