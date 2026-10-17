/*
 * Memory card images in the standard raw 128 KiB layout (compatible with
 * emulator .mcr files): block 0 holds the header and 15 directory frames,
 * blocks 1-15 hold save data (8 KiB each).
 */
#include <stdlib.h>
#include <SDL3/SDL_filesystem.h>

#include "memcard.h"
#include "psx.h"

#define CARD_SIZE 0x20000
#define FRAME 128
#define BLOCK 8192

static uint8_t g_card[2][CARD_SIZE];
static bool g_loaded[2];
static bool g_dirty[2];

static void card_path(int port, char *out, size_t n) {
    snprintf(out, n, "%s/memcard%d.mcr", platform_data_dir(), port);
}

static void frame_checksum(uint8_t *f) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++)
        x ^= f[i];
    f[127] = x;
}

static void card_format_mem(uint8_t *card) {
    memset(card, 0, CARD_SIZE);
    card[0] = 'M';
    card[1] = 'C';
    frame_checksum(card);
    for (int i = 1; i < 16; i++) {
        uint8_t *f = card + i * FRAME;
        f[0] = 0xA0;
        f[8] = 0xFF;
        f[9] = 0xFF;
        frame_checksum(f);
    }
    for (int i = 16; i < 36; i++) {
        uint8_t *f = card + i * FRAME;
        memset(f, 0, FRAME);
        f[0] = f[1] = f[2] = f[3] = 0xFF;
        f[8] = f[9] = 0xFF;
        frame_checksum(f);
    }
    card[63 * FRAME] = 'M';
    card[63 * FRAME + 1] = 'C';
    frame_checksum(card + 63 * FRAME);
}

static void card_load(int port) {
    if (g_loaded[port])
        return;
    char path[1024];
    card_path(port, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f && fread(g_card[port], 1, CARD_SIZE, f) == CARD_SIZE &&
        g_card[port][0] == 'M' && g_card[port][1] == 'C') {
        LOGI("card", "loaded %s", path);
    } else {
        card_format_mem(g_card[port]);
        g_dirty[port] = true;
        LOGI("card", "created blank memory card %s", path);
    }
    if (f)
        fclose(f);
    g_loaded[port] = true;
    memcard_flush(port);
}

void memcard_flush(int port) {
    if (!g_dirty[port])
        return;
    char path[1024];
    card_path(port, path, sizeof path);
    SDL_CreateDirectory(platform_data_dir());
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOGE("card", "cannot write %s", path);
        return;
    }
    fwrite(g_card[port], 1, CARD_SIZE, f);
    fclose(f);
    g_dirty[port] = false;
}

void memcard_init_all(void) {
    card_load(0);
    card_load(1);
}

bool memcard_present(int port) {
    /* only the first slot has a card inserted */
    card_load(port & 1);
    return (port & 1) == 0;
}

static uint8_t *dir_frame(int port, int idx) {
    return g_card[port] + (idx + 1) * FRAME;
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool memcard_dir_entry(int port, int idx, char *name, uint32_t *size) {
    card_load(port);
    uint8_t *f = dir_frame(port, idx);
    if (f[0] != 0x51)
        return false;
    memcpy(name, f + 10, 20);
    name[20] = 0;
    *size = rd32le(f + 4);
    return true;
}

bool memcard_match(const char *pat, const char *name) {
    if (!*pat)
        return !*name;
    if (*pat == '*')
        return true;
    if (*pat == '?' ? *name != 0 : *pat == *name)
        return memcard_match(pat + 1, name + 1);
    return false;
}

int memcard_find(int port, const char *name, uint32_t *size) {
    card_load(port);
    for (int i = 0; i < 15; i++) {
        uint8_t *f = dir_frame(port, i);
        if (f[0] == 0x51 && !strncmp((const char *)f + 10, name, 20)) {
            *size = rd32le(f + 4);
            return i;
        }
    }
    return -1;
}

bool memcard_file_size(int port, int slot, uint32_t *size) {
    uint8_t *f = dir_frame(port, slot);
    *size = rd32le(f + 4);
    return true;
}

static int block_of(int port, int slot, uint32_t off) {
    int blk = slot;
    for (uint32_t k = off / BLOCK; k > 0; k--) {
        uint8_t *f = dir_frame(port, blk);
        int next = f[8] | (f[9] << 8);
        if (next == 0xFFFF)
            return -1;
        blk = next;
    }
    return blk;
}

void memcard_file_io(int port, int slot, uint32_t off, uint8_t *buf,
                     uint32_t len, bool write) {
    for (uint32_t i = 0; i < len; i++) {
        int blk = block_of(port, slot, off + i);
        if (blk < 0)
            return;
        uint8_t *p = g_card[port] + (blk + 1) * BLOCK + ((off + i) % BLOCK);
        if (write) {
            *p = buf[i];
            g_dirty[port] = true;
        } else {
            buf[i] = *p;
        }
    }
}

int memcard_create(int port, const char *name, int blocks) {
    card_load(port);
    uint32_t dummy;
    if (memcard_find(port, name, &dummy) >= 0)
        return -1;
    int free_idx[15], nf = 0;
    for (int i = 0; i < 15; i++) {
        uint8_t s = dir_frame(port, i)[0];
        if (s == 0xA0 || s == 0xA1 || s == 0xA2 || s == 0xA3)
            free_idx[nf++] = i;
    }
    if (nf < blocks)
        return -1;
    for (int k = 0; k < blocks; k++) {
        uint8_t *f = dir_frame(port, free_idx[k]);
        memset(f, 0, FRAME);
        f[0] = (k == 0) ? 0x51 : (k == blocks - 1 ? 0x53 : 0x52);
        if (k == 0 && blocks == 1)
            f[0] = 0x51;
        if (k == 0) {
            wr32le(f + 4, (uint32_t)blocks * BLOCK);
            snprintf((char *)f + 10, 21, "%s", name);
        }
        int next = (k + 1 < blocks) ? free_idx[k + 1] : 0xFFFF;
        f[8] = (uint8_t)next;
        f[9] = (uint8_t)(next >> 8);
        frame_checksum(f);
        memset(g_card[port] + (free_idx[k] + 1) * BLOCK, 0, BLOCK);
    }
    g_dirty[port] = true;
    memcard_flush(port);
    LOGI("card", "created %s (%d blocks) at slot %d", name, blocks,
         free_idx[0]);
    return free_idx[0];
}

bool memcard_delete(int port, const char *name) {
    uint32_t size;
    int blk = memcard_find(port, name, &size);
    if (blk < 0)
        return false;
    while (blk >= 0 && blk < 15) {
        uint8_t *f = dir_frame(port, blk);
        int next = f[8] | (f[9] << 8);
        f[0] = (uint8_t)(0xA0 | (f[0] & 0x0F));
        frame_checksum(f);
        blk = next == 0xFFFF ? -1 : next;
    }
    g_dirty[port] = true;
    memcard_flush(port);
    return true;
}

bool memcard_rename(int port, const char *from, const char *to) {
    uint32_t size;
    int blk = memcard_find(port, from, &size);
    if (blk < 0)
        return false;
    uint8_t *f = dir_frame(port, blk);
    memset(f + 10, 0, 21);
    snprintf((char *)f + 10, 21, "%s", to);
    frame_checksum(f);
    g_dirty[port] = true;
    memcard_flush(port);
    return true;
}

bool memcard_format(int port) {
    card_format_mem(g_card[port]);
    g_loaded[port] = true;
    g_dirty[port] = true;
    memcard_flush(port);
    return true;
}

void memcard_sector_io(int port, int sector, uint8_t *buf, bool write) {
    card_load(port);
    if (sector < 0 || sector >= CARD_SIZE / FRAME) {
        memset(buf, 0, FRAME);
        return;
    }
    uint8_t *p = g_card[port] + sector * FRAME;
    if (write) {
        memcpy(p, buf, FRAME);
        g_dirty[port] = true;
        memcard_flush(port);
    } else {
        memcpy(buf, p, FRAME);
    }
}
