/* Minimal PNG writer (stored deflate blocks, no compression library). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(const uint8_t *buf, size_t len, uint32_t c) {
    c = ~c;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    put32(f, len);
    fwrite(type, 1, 4, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t c = crc((const uint8_t *)type, 4, 0);
    c = crc(data, len, c);
    put32(f, c);
}

/* rgb: w*h*3 bytes */
int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h) {
    static int init;
    if (!init) {
        crc_init();
        init = 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0};
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (size_t)h * (size_t)(w * 3 + 1);
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen);
    size_t zp = 0;
    z[zp++] = 0x78;
    z[zp++] = 0x01;
    /* build the raw scanline stream on the fly */
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (size_t)(w * 3 + 1)] = 0;
        memcpy(raw + (size_t)y * (size_t)(w * 3 + 1) + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[zp++] = (off + n >= raw_len) ? 1 : 0;
        z[zp++] = (uint8_t)n;
        z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)~n;
        z[zp++] = (uint8_t)(~n >> 8);
        memcpy(z + zp, raw + off, n);
        zp += n;
    }
    uint32_t adler = (b << 16) | a;
    z[zp++] = (uint8_t)(adler >> 24);
    z[zp++] = (uint8_t)(adler >> 16);
    z[zp++] = (uint8_t)(adler >> 8);
    z[zp++] = (uint8_t)adler;
    chunk(f, "IDAT", z, (uint32_t)zp);
    chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    fclose(f);
    return 0;
}
