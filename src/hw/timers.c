/*
 * Root counters 0-2. Counters are evaluated lazily from the CPU cycle
 * count; interrupts are scheduled for the next target/overflow crossing.
 */
#include "psx.h"
#include "savestate.h"

typedef struct {
    uint32_t mode;
    uint32_t target;
    int64_t base;      /* tick count corresponding to counter value 0 */
    bool irq_fired;    /* one-shot latch */
} Timer;

static Timer g_t[3];
static const EventId k_ev[3] = {EV_TIMER0, EV_TIMER1, EV_TIMER2};

/* Ticks of timer n's clock source at CPU cycle `cyc`. */
static int64_t ticks_at(int n, int64_t cyc) {
    uint32_t src = (g_t[n].mode >> 8) & 3;
    if (n == 1 && (src & 1))
        return cyc / PSX_HBLANK_CYCLES;
    if (n == 2 && (src & 2))
        return cyc / 8;
    if (n == 0 && (src & 1))
        return cyc * 2 / 5; /* approximate 320-mode dot clock */
    return cyc;
}

static int64_t cycles_for_ticks(int n, int64_t ticks) {
    uint32_t src = (g_t[n].mode >> 8) & 3;
    if (n == 1 && (src & 1))
        return ticks * PSX_HBLANK_CYCLES;
    if (n == 2 && (src & 2))
        return ticks * 8;
    if (n == 0 && (src & 1))
        return ticks * 5 / 2;
    return ticks;
}

static uint32_t period(int n) {
    return (g_t[n].mode & 0x8) ? (g_t[n].target ? g_t[n].target + 1 : 0x10000)
                               : 0x10000;
}

static uint32_t counter_value(int n) {
    int64_t t = ticks_at(n, now_cycles()) - g_t[n].base;
    if (t < 0)
        t = 0;
    return (uint32_t)(t % period(n)) & 0xFFFF;
}

static void schedule(int n);

static void timer_event_n(int n, int64_t now) {
    Timer *t = &g_t[n];
    uint32_t m = t->mode;
    int64_t elapsed = ticks_at(n, now) - t->base;
    bool target_hit = (m & 0x10) && t->target &&
                      (elapsed % period(n)) >= 0;
    (void)target_hit;
    if (m & 0x10)
        t->mode |= 1u << 11;
    if (!(m & 0x8) && (m & 0x20))
        t->mode |= 1u << 12;
    bool repeat = (m >> 6) & 1;
    if (!t->irq_fired || repeat) {
        irq_raise(IRQ_TIMER0 + n);
        t->irq_fired = true;
        t->mode &= ~(1u << 10); /* IRQ requested (active low) */
    }
    schedule(n);
}

static void ev0(int64_t now, int64_t late) { (void)late; timer_event_n(0, now); }
static void ev1(int64_t now, int64_t late) { (void)late; timer_event_n(1, now); }
static void ev2(int64_t now, int64_t late) { (void)late; timer_event_n(2, now); }

static void schedule(int n) {
    Timer *t = &g_t[n];
    uint32_t m = t->mode;
    bool want = (m & 0x30) != 0;
    if (!want || (t->irq_fired && !((m >> 6) & 1))) {
        sched_cancel(k_ev[n]);
        return;
    }
    if (n == 2 && (m & 1) && ((m >> 1) & 3) % 3 == 0) {
        sched_cancel(k_ev[n]); /* stopped */
        return;
    }
    int64_t now_t = ticks_at(n, now_cycles());
    int64_t elapsed = now_t - t->base;
    int64_t p = period(n);
    int64_t goal;
    if ((m & 0x10) && t->target) {
        int64_t cycle_start = elapsed - (elapsed % p);
        goal = cycle_start + t->target;
        if (goal <= elapsed)
            goal += p;
    } else {
        goal = elapsed - (elapsed % p) + p;
    }
    sched_at(k_ev[n], cycles_for_ticks(n, t->base + goal) + 1);
}

void timers_state(StateIO *s) {
    state_section(s, "TIMR");
    STATE(s, g_t);
}

void timers_init(void) {
    memset(g_t, 0, sizeof g_t);
    for (int i = 0; i < 3; i++)
        g_t[i].mode = 1u << 10;
    sched_register(EV_TIMER0, ev0);
    sched_register(EV_TIMER1, ev1);
    sched_register(EV_TIMER2, ev2);
}

uint32_t timers_read(uint32_t addr) {
    int n = (int)((addr >> 4) & 3);
    if (n > 2)
        return 0;
    switch (addr & 0xF) {
    case 0x0:
        return counter_value(n);
    case 0x4: {
        uint32_t v = g_t[n].mode;
        g_t[n].mode &= ~((1u << 11) | (1u << 12));
        return v;
    }
    case 0x8:
        return g_t[n].target;
    default:
        return 0;
    }
}

void timers_write(uint32_t addr, uint32_t v) {
    int n = (int)((addr >> 4) & 3);
    if (n > 2)
        return;
    switch (addr & 0xF) {
    case 0x0:
        g_t[n].base = ticks_at(n, now_cycles()) - (v & 0xFFFF);
        break;
    case 0x4:
        g_t[n].mode = (v & 0x3FF) | (1u << 10);
        g_t[n].irq_fired = false;
        g_t[n].base = ticks_at(n, now_cycles());
        break;
    case 0x8:
        g_t[n].target = v & 0xFFFF;
        break;
    }
    schedule(n);
}

void timers_hblank(void) {}
void timers_vblank(bool start) { (void)start; }
