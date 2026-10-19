/*
 * Memory bus slow path: hardware I/O registers (0x1F801000..), expansion
 * regions, BIOS ROM and the cache control register.
 */
#include "psx.h"
#include "savestate.h"

uint8_t g_ram[0x200000];
uint8_t g_scratch[0x400];

static uint32_t g_memctrl[16];
static uint32_t g_ramsize = 0x00000B88;
static uint32_t g_cachectrl;

void bus_state(StateIO *s) {
    state_section(s, "MEM ");
    STATE(s, g_ram);
    STATE(s, g_scratch);
    STATE(s, g_memctrl);
    STATE(s, g_ramsize);
    STATE(s, g_cachectrl);
}

static void unmapped(const char *what, uint32_t addr, uint32_t v) {
    static int count;
    if (count++ < 64)
        LOGW("bus", "unmapped %s %08X (val %08X) pc~%08X", what, addr, v,
             g_cpu ? g_cpu->pc : 0);
}

static uint32_t io_read(uint32_t p, int size) {
    if (p >= 0x1F801000 && p < 0x1F801024)
        return g_memctrl[(p - 0x1F801000) >> 2];
    if (p >= 0x1F801040 && p < 0x1F801050)
        return sio_read(p, size);
    if (p >= 0x1F801050 && p < 0x1F801060)
        return 0; /* SIO1: not connected */
    if (p == 0x1F801060)
        return g_ramsize;
    if (p == 0x1F801070)
        return g_istat;
    if (p == 0x1F801072)
        return 0;
    if (p == 0x1F801074)
        return g_imask;
    if (p == 0x1F801076)
        return 0;
    if (p >= 0x1F801080 && p < 0x1F801100)
        return dma_read(p & ~3u);
    if (p >= 0x1F801100 && p < 0x1F801130)
        return timers_read(p);
    if (p >= 0x1F801800 && p < 0x1F801804)
        return cdrom_read(p);
    if (p == 0x1F801810)
        return gpu_read_data();
    if (p == 0x1F801814)
        return gpu_read_stat();
    if (p == 0x1F801820 || p == 0x1F801824)
        return mdec_read(p);
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4)
            return spu_read16(p) | (spu_read16(p + 2) << 16);
        return spu_read16(p & ~1u);
    }
    if (p >= 0x1F802000 && p < 0x1F803000)
        return 0; /* expansion 2 (debug/POST) */
    unmapped("read", p, 0);
    return 0;
}

static void io_write(uint32_t p, uint32_t v, int size) {
    if (p >= 0x1F801000 && p < 0x1F801024) {
        g_memctrl[(p - 0x1F801000) >> 2] = v;
        return;
    }
    if (p >= 0x1F801040 && p < 0x1F801050) {
        sio_write(p, v, size);
        return;
    }
    if (p >= 0x1F801050 && p < 0x1F801060)
        return;
    if (p == 0x1F801060) {
        g_ramsize = v;
        return;
    }
    if (p == 0x1F801070) {
        LOGD("irq", "I_STAT &= %03X (was %03X)", v & 0x7FF, g_istat);
        g_istat &= v;
        cpu_request_check();
        return;
    }
    if (p == 0x1F801074) {
        LOGD("irq", "I_MASK = %03X", v & 0x7FF);
        g_imask = v & 0x7FF;
        cpu_request_check();
        return;
    }
    if (p == 0x1F801072 || p == 0x1F801076)
        return;
    if (p >= 0x1F801080 && p < 0x1F801100) {
        dma_write_sized(p, v, size);
        return;
    }
    if (p >= 0x1F801100 && p < 0x1F801130) {
        timers_write(p, v);
        return;
    }
    if (p >= 0x1F801800 && p < 0x1F801804) {
        cdrom_write(p, v & 0xFF);
        return;
    }
    if (p == 0x1F801810) {
        gpu_write_gp0(v);
        return;
    }
    if (p == 0x1F801814) {
        gpu_write_gp1(v);
        return;
    }
    if (p == 0x1F801820 || p == 0x1F801824) {
        mdec_write(p, v);
        return;
    }
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) {
            spu_write16(p, v & 0xFFFF);
            spu_write16(p + 2, v >> 16);
        } else {
            spu_write16(p & ~1u, v & 0xFFFF);
        }
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F803000)
        return;
    unmapped("write", p, v);
}

static uint32_t bus_read(uint32_t addr, int size) {
    if (addr == 0xFFFE0130)
        return g_cachectrl;
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p >= 0x1F000000 && p < 0x1F800000)
        return size == 1 ? 0xFF : (size == 2 ? 0xFFFF : 0xFFFFFFFFu);
    if (p >= 0x1FC00000 && p < 0x1FC80000)
        return 0; /* no BIOS ROM: HLE */
    if (p >= 0x1F801080 && p < 0x1F801100) {
        /* 32-bit DMA registers: extract the addressed byte lanes */
        uint32_t v = dma_read(p & ~3u) >> ((p & 3) * 8);
        return size == 1 ? (v & 0xFF) : (size == 2 ? (v & 0xFFFF) : v);
    }
    if (p >= 0x1F801000 && p < 0x1F803000) {
        uint32_t v = io_read(p & ~(uint32_t)(size == 4 ? 3 : 0), size);
        if (size == 1) {
            /* CD-ROM and SIO are byte registers; others extract a byte */
            if ((p >= 0x1F801800 && p < 0x1F801804) ||
                (p >= 0x1F801040 && p < 0x1F801050))
                return v & 0xFF;
            return (v >> ((p & 1) * 8)) & 0xFF;
        }
        if (size == 2)
            return v & 0xFFFF;
        return v;
    }
    unmapped("read", addr, 0);
    return 0;
}

static void bus_write(uint32_t addr, uint32_t v, int size) {
    if (addr == 0xFFFE0130) {
        g_cachectrl = v;
        return;
    }
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p >= 0x1F801000 && p < 0x1F803000) {
        io_write(p, v, size);
        return;
    }
    if (p >= 0x1F000000 && p < 0x1F800000)
        return;
    if (p >= 0x1FC00000 && p < 0x1FC80000)
        return;
    unmapped("write", addr, v);
}

uint32_t bus_read32(uint32_t a) { return bus_read(a, 4); }
uint32_t bus_read16(uint32_t a) { return bus_read(a, 2); }
uint32_t bus_read8(uint32_t a) { return bus_read(a, 1); }
void bus_write32(uint32_t a, uint32_t v) { bus_write(a, v, 4); }
void bus_write16(uint32_t a, uint32_t v) { bus_write(a, v & 0xFFFF, 2); }
void bus_write8(uint32_t a, uint32_t v) { bus_write(a, v & 0xFF, 1); }
