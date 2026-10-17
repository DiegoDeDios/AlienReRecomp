/*
 * SIO0: controller port with a DualShock (SCPH-1200) model, including the
 * configuration-mode commands libpad uses to switch to analog mode and set
 * up vibration. Memory cards are handled at the BIOS level, so the card
 * address (0x81) never answers here.
 */
#include "psx.h"

typedef struct {
    uint16_t buttons;  /* 1 = pressed */
    uint8_t rx, ry, lx, ly;
    bool analog;
    bool config;
    bool locked;
    uint8_t motor_map[6];
    uint8_t motor[2];
} Pad;

static Pad g_pad[2];

static struct {
    uint16_t mode, ctrl, baud;
    uint32_t stat_irq;     /* bit 9 IRQ flag */
    uint8_t rx;
    bool rx_ready;
    bool ack_pending;
    /* transaction state */
    int port;
    int pos;
    uint8_t device;        /* 0x01 pad, 0x81 card, 0 none */
    uint8_t cmd;
    uint8_t resp[32];
    int resp_len;
    uint8_t txbuf[32];
} S;

void pad_set_buttons(int port, uint16_t pressed) { g_pad[port & 1].buttons = pressed; }

void pad_set_analog(int port, uint8_t rx, uint8_t ry, uint8_t lx, uint8_t ly) {
    Pad *p = &g_pad[port & 1];
    p->rx = rx, p->ry = ry, p->lx = lx, p->ly = ly;
}

void pad_get_sticks(int port, uint8_t *rx, uint8_t *ry, uint8_t *lx, uint8_t *ly) {
    const Pad *p = &g_pad[port & 1];
    *rx = p->rx, *ry = p->ry, *lx = p->lx, *ly = p->ly;
}

static void sio_ack_event(int64_t now, int64_t late) {
    (void)now, (void)late;
    if (S.ack_pending) {
        S.ack_pending = false;
        if (S.ctrl & (1u << 12)) {
            S.stat_irq = 1;
            irq_raise(IRQ_SIO0);
        }
    }
}

void sio_init(void) {
    memset(&S, 0, sizeof S);
    memset(g_pad, 0, sizeof g_pad);
    for (int i = 0; i < 2; i++) {
        g_pad[i].rx = g_pad[i].ry = g_pad[i].lx = g_pad[i].ly = 0x80;
        memset(g_pad[i].motor_map, 0xFF, 6);
    }
    sched_register(EV_SIO, sio_ack_event);
}

static uint8_t pad_id(const Pad *p) {
    if (p->config)
        return 0xF3;
    return p->analog ? 0x73 : 0x41;
}

/* Build the full response for a pad command once the command byte is
 * known. Index 0 = reply to address byte, 1 = reply to command byte. */
static void pad_build_response(Pad *p, uint8_t cmd) {
    uint8_t *r = S.resp;
    uint16_t b = (uint16_t)~p->buttons;
    r[0] = 0xFF;
    r[2] = 0x5A;
    int n;
    if (p->config && cmd != 0x42 && cmd != 0x43) {
        r[1] = 0xF3;
        memset(r + 3, 0, 6);
        n = 9;
        switch (cmd) {
        case 0x44: break;
        case 0x45:
            r[3] = 0x03; r[4] = 0x02; r[5] = p->analog ? 1 : 0;
            r[6] = 0x02; r[7] = 0x01; r[8] = 0x00;
            break;
        case 0x46: /* depends on parameter byte; filled on the fly */
        case 0x47:
        case 0x4C:
            break;
        case 0x4D:
            memcpy(r + 3, p->motor_map, 6);
            break;
        default:
            break;
        }
        S.resp_len = n;
        return;
    }
    r[1] = pad_id(p);
    r[3] = (uint8_t)b;
    r[4] = (uint8_t)(b >> 8);
    n = 5;
    if (p->analog || p->config) {
        if (p->config && cmd == 0x43) {
            memset(r + 3, 0, 6);
        } else {
            r[5] = p->rx;
            r[6] = p->ry;
            r[7] = p->lx;
            r[8] = p->ly;
        }
        n = 9;
    }
    S.resp_len = n;
}

/* parameter-dependent replies and command side effects */
static void pad_param(Pad *p, int pos, uint8_t tx) {
    uint8_t cmd = S.cmd;
    S.txbuf[pos] = tx;
    if (cmd == 0x46 && pos == 3) {
        if (tx == 0) { uint8_t v[6] = {0, 0, 1, 2, 0, 0x0A}; memcpy(S.resp + 3, v, 6); }
        else { uint8_t v[6] = {0, 0, 1, 1, 1, 0x14}; memcpy(S.resp + 3, v, 6); }
    }
    if (cmd == 0x47 && pos == 3) {
        uint8_t v[6] = {0, 0, 2, 0, 1, 0};
        memcpy(S.resp + 3, v, 6);
    }
    if (cmd == 0x4C && pos == 3) {
        uint8_t v[6] = {0, 0, 0, (uint8_t)(tx == 0 ? 4 : 7), 0, 0};
        memcpy(S.resp + 3, v, 6);
    }
}

static void pad_finish(Pad *p) {
    uint8_t cmd = S.cmd;
    switch (cmd) {
    case 0x42:
        /* vibration values through the mapping */
        for (int i = 0; i < 6; i++) {
            if (p->motor_map[i] == 0) p->motor[0] = S.txbuf[3 + i];
            if (p->motor_map[i] == 1) p->motor[1] = S.txbuf[3 + i];
        }
        break;
    case 0x43:
        if (p->config != (S.txbuf[3] == 1))
            LOGD("pad", "config mode %s", S.txbuf[3] == 1 ? "enter" : "exit");
        p->config = S.txbuf[3] == 1;
        break;
    case 0x44:
        p->analog = S.txbuf[3] == 1;
        p->locked = S.txbuf[4] == 3;
        LOGD("pad", "set mode: %s%s", p->analog ? "analog" : "digital",
             p->locked ? " (locked)" : "");
        break;
    case 0x4D:
        memcpy(p->motor_map, S.txbuf + 3, 6);
        LOGD("pad", "vibration map %02X %02X %02X %02X %02X %02X", p->motor_map[0],
             p->motor_map[1], p->motor_map[2], p->motor_map[3], p->motor_map[4], p->motor_map[5]);
        break;
    default:
        break;
    }
}

static uint8_t sio_exchange(uint8_t tx) {
    Pad *p = &g_pad[S.port];
    uint8_t out = 0xFF;
    bool ack = false;
    if (S.pos == 0) {
        S.device = tx;
        if (tx == 0x01 && S.port == 0) {
            out = 0xFF;
            ack = true;
        } else {
            S.device = 0; /* nothing answers (card / port 2) */
        }
    } else if (S.device == 0x01) {
        if (S.pos == 1) {
            S.cmd = tx;
            pad_build_response(p, tx);
        } else {
            pad_param(p, S.pos, tx);
        }
        out = S.pos < S.resp_len ? S.resp[S.pos] : 0xFF;
        ack = S.pos + 1 < S.resp_len;
        if (!ack)
            pad_finish(p);
    }
    LOGD("sio", "port%d pos%d tx=%02X rx=%02X ack=%d ctrl=%04X", S.port, S.pos, tx, out, ack, S.ctrl);
    S.pos++;
    if (ack) {
        S.ack_pending = true;
        sched_in(EV_SIO, 340);
    }
    return out;
}

uint32_t sio_read(uint32_t addr, int size) {
    (void)size;
    switch (addr & 0xF) {
    case 0x0: {
        uint8_t v = S.rx;
        S.rx_ready = false;
        S.rx = 0xFF;
        return v | (v << 8) | (v << 16) | ((uint32_t)v << 24);
    }
    case 0x4: {
        uint32_t st = 0x5;                      /* TX ready 1 and 2 */
        st |= (uint32_t)S.rx_ready << 1;
        st |= (uint32_t)S.ack_pending << 7;     /* /ACK low while pending */
        st |= S.stat_irq << 9;
        return st;
    }
    case 0x8: return S.mode;
    case 0xA: return S.ctrl;
    case 0xE: return S.baud;
    default: return 0;
    }
}

void sio_write(uint32_t addr, uint32_t v, int size) {
    (void)size;
    switch (addr & 0xF) {
    case 0x0:
        if (S.ctrl & 0x0002) { /* /JOY selected */
            S.rx = sio_exchange((uint8_t)v);
        } else {
            S.rx = 0xFF;
        }
        S.rx_ready = true;
        break;
    case 0x8: S.mode = (uint16_t)v; break;
    case 0xA: {
        uint16_t old = S.ctrl;
        S.ctrl = (uint16_t)v;
        if (v & 0x10) { /* acknowledge */
            S.stat_irq = 0;
            S.ctrl &= (uint16_t)~0x10;
        }
        if (v & 0x40) { /* reset */
            S.stat_irq = 0;
            S.rx_ready = false;
            S.pos = 0;
            S.ctrl = 0;
        }
        int port = (v >> 13) & 1;
        bool selected = (v & 2) != 0;
        if (!selected || !(old & 2) || port != S.port) {
            /* deselect or new selection ends the transaction */
            S.pos = 0;
            S.port = port;
            S.ack_pending = false;
        }
        break;
    }
    case 0xE: S.baud = (uint16_t)v; break;
    default: break;
    }
}
