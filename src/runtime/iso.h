#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ISO9660 access on the opened disc image (2048-byte user data). */
bool iso_find(const char *path, uint32_t *lba, uint32_t *size);
bool iso_read(uint32_t lba, uint8_t *dst, uint32_t size);
