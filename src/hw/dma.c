/*
 * DMA controller (7 channels). Transfers are performed immediately; the
 * channel stays busy for a short, size-dependent time after which the
 * completion interrupt is raised (as the libraries expect asynchronous
 * completion).
 */
#include "psx.h"

enum { CH_MDEC_IN, CH_MDEC_OUT, CH_GPU, CH_CDROM, CH_SPU, CH_PIO, CH_OTC };

typedef struct {
    uint32_t madr, bcr, chcr;
    int64_t done_at;
    bool busy;
} DmaChan;

static DmaChan g_ch[7];
static uint32_t g_dpcr = 0x07654321;
static uint32_t g_dicr;

static void dicr_update_master(void) {
    bool force = (g_dicr >> 15) & 1;
    bool en = (g_dicr >> 23) & 1;
    uint32_t flags = (g_dicr >> 24) & 0x7F;
    uint32_t enables = (g_dicr >> 16) & 0x7F;
    bool master = force || (en && (flags & enables));
    bool was = (g_dicr >> 31) & 1;
    g_dicr = (g_dicr & 0x7FFFFFFFu) | ((uint32_t)master << 31);
    if (master && !was)
        irq_raise(IRQ_DMA);
}

void dma_irq_complete(int ch) {
    LOGD("dma", "ch%d complete, DICR=%08X", ch, g_dicr);
    if (g_dicr & (1u << (16 + ch)))
        g_dicr |= 1u << (24 + ch);
    dicr_update_master();
}

static void dma_schedule(void);

static void dma_event(int64_t now, int64_t late) {
    (void)late;
    for (int ch = 0; ch < 7; ch++) {
        DmaChan *d = &g_ch[ch];
        if (d->busy && d->done_at <= now) {
            d->busy = false;
            d->chcr &= ~((1u << 24) | (1u << 28));
            dma_irq_complete(ch);
        }
    }
    dma_schedule();
}

static void dma_schedule(void) {
    int64_t next = INT64_MAX;
    for (int ch = 0; ch < 7; ch++)
        if (g_ch[ch].busy && g_ch[ch].done_at < next)
            next = g_ch[ch].done_at;
    if (next != INT64_MAX)
        sched_at(EV_DMA, next);
    else
        sched_cancel(EV_DMA);
}

void dma_init(void) {
    memset(g_ch, 0, sizeof g_ch);
    g_dpcr = 0x07654321;
    g_dicr = 0;
    sched_register(EV_DMA, dma_event);
}

static uint32_t block_words(const DmaChan *d) {
    uint32_t sync = (d->chcr >> 9) & 3;
    if (sync == 0) {
        uint32_t n = d->bcr & 0xFFFF;
        return n ? n : 0x10000;
    }
    uint32_t bs = d->bcr & 0xFFFF, ba = d->bcr >> 16;
    if (!bs)
        bs = 0x10000;
    return bs * ba;
}

static void dma_run(int ch) {
    DmaChan *d = &g_ch[ch];
    uint32_t sync = (d->chcr >> 9) & 3;
    bool from_ram = d->chcr & 1;
    uint32_t addr = d->madr & 0x1FFFFC;
    uint32_t words = block_words(d);
    int64_t cost = 16;

    switch (ch) {
    case CH_GPU:
        if (sync == 2) {
            cost += gpu_dma_linked_list(addr);
            d->madr = 0x00FFFFFF;
        } else if (from_ram) {
            gpu_dma_write(addr, words);
            cost += words;
            if (sync == 1)
                d->madr = (addr + words * 4) & 0x1FFFFC;
        } else {
            gpu_dma_read(addr, words);
            cost += words;
            if (sync == 1)
                d->madr = (addr + words * 4) & 0x1FFFFC;
        }
        break;
    case CH_OTC: {
        /* reverse-clear ordering table: each entry points to the previous */
        uint32_t n = d->bcr & 0xFFFF;
        if (!n)
            n = 0x10000;
        {
            void gpu_note_ot(uint32_t lo, uint32_t hi);
            gpu_note_ot(addr - (n - 1) * 4, addr);
        }

        uint32_t a = addr;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = (i == n - 1) ? 0x00FFFFFFu : ((a - 4) & 0x1FFFFF);
            mem_w32(a, v);
            a = (a - 4) & 0x1FFFFC;
        }
        cost += n;
        break;
    }
    case CH_SPU:
        if (from_ram)
            spu_dma_write(addr, words);
        else
            spu_dma_read(addr, words);
        cost += words * 4;
        if (sync == 1)
            d->madr = (addr + words * 4) & 0x1FFFFC;
        break;
    case CH_CDROM:
        cdrom_dma_read(addr, words);
        cost += words * 6;
        if (sync == 1)
            d->madr = (addr + words * 4) & 0x1FFFFC;
        break;
    case CH_MDEC_IN:
        mdec_dma_in(addr, words);
        cost += words;
        if (sync == 1)
            d->madr = (addr + words * 4) & 0x1FFFFC;
        break;
    case CH_MDEC_OUT:
        mdec_dma_out(addr, words);
        cost += words * 2;
        if (sync == 1)
            d->madr = (addr + words * 4) & 0x1FFFFC;
        break;
    default:
        LOGW("dma", "transfer on unsupported channel %d", ch);
        break;
    }
    if (sync == 0) {
        /* burst mode: the CPU is halted until the transfer completes */
        g_cpu->cycles += cost;
        d->busy = false;
        d->chcr &= ~((1u << 24) | (1u << 28));
        dma_irq_complete(ch);
        return;
    }
    d->busy = true;
    d->done_at = now_cycles() + cost;
    dma_schedule();
}

static void dma_maybe_start(int ch) {
    DmaChan *d = &g_ch[ch];
    if (!(g_dpcr & (8u << (ch * 4))))
        return;
    uint32_t sync = (d->chcr >> 9) & 3;
    bool start = (d->chcr >> 24) & 1;
    bool trigger = (d->chcr >> 28) & 1;
    if (!start || d->busy)
        return;
    if (sync == 0 && !trigger)
        return;
    d->chcr &= ~(1u << 28);
    dma_run(ch);
}

uint32_t dma_read(uint32_t addr) {
    uint32_t off = addr - 0x1F801080;
    if (addr == 0x1F8010F0)
        return g_dpcr;
    if (addr == 0x1F8010F4)
        return g_dicr;
    if (addr >= 0x1F8010F0)
        return 0;
    int ch = (int)(off >> 4);
    switch (off & 0xC) {
    case 0x0: return g_ch[ch].madr;
    case 0x4: return g_ch[ch].bcr;
    case 0x8: return g_ch[ch].chcr;
    default: return 0;
    }
}

/* 8/16-bit stores update only their byte lanes of the 32-bit register */
void dma_write_sized(uint32_t addr, uint32_t v, int size) {
    if (size == 4) {
        dma_write(addr & ~3u, v);
        return;
    }
    uint32_t reg = addr & ~3u;
    int shift = (int)(addr & 3) * 8;
    uint32_t lane = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
    uint32_t val = (v << shift) & lane;
    if (reg == 0x1F8010F4) {
        /* keep other enables; ack only flags inside the written lanes */
        uint32_t cur = g_dicr & ~0x7F000000u & ~0x80000000u;
        dma_write(reg, (cur & ~lane) | val);
        return;
    }
    uint32_t cur = dma_read(reg);
    dma_write(reg, (cur & ~lane) | val);
}

void dma_write(uint32_t addr, uint32_t v) {
    uint32_t off = addr - 0x1F801080;
    if (addr == 0x1F8010F0) {
        LOGD("dma", "DPCR = %08X", v);
        g_dpcr = v;
        for (int ch = 0; ch < 7; ch++)
            dma_maybe_start(ch);
        return;
    }
    if (addr == 0x1F8010F4) {
        LOGD("dma", "DICR = %08X (was %08X)", v, g_dicr);
        /* bits 24-30 are write-1-to-clear flags */
        uint32_t flags = (g_dicr & 0x7F000000u) & ~(v & 0x7F000000u);
        g_dicr = (v & 0x00FF803Fu) | flags;
        dicr_update_master();
        return;
    }
    if (addr >= 0x1F8010F0)
        return;
    int ch = (int)(off >> 4);
    switch (off & 0xC) {
    case 0x0: g_ch[ch].madr = v & 0xFFFFFF; break;
    case 0x4: g_ch[ch].bcr = v; break;
    case 0x8:
        LOGD("dma", "ch%d CHCR = %08X madr=%08X bcr=%08X dpcr=%08X", ch, v, g_ch[ch].madr, g_ch[ch].bcr, g_dpcr);
        if (ch == CH_OTC)
            v = (v & 0x51000000u) | 0x2;
        if (g_ch[ch].busy) {
            if ((v >> 24) & 1) {
                /* restart while the previous transfer's completion is still
                 * pending: its data already moved, so retire it now */
                g_ch[ch].busy = false;
                dma_irq_complete(ch);
            } else {
                g_ch[ch].busy = false; /* stop request */
            }
        }
        g_ch[ch].chcr = v;
        dma_maybe_start(ch);
        break;
    }
}
