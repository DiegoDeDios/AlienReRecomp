#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Memory card storage: standard 128 KiB raw images (.mcr), one per port. */
void memcard_init_all(void);
bool memcard_present(int port);
int memcard_create(int port, const char *name, int blocks);
int memcard_find(int port, const char *name, uint32_t *size);
bool memcard_file_size(int port, int slot, uint32_t *size);
void memcard_file_io(int port, int slot, uint32_t off, uint8_t *buf,
                     uint32_t len, bool write);
void memcard_flush(int port);
bool memcard_dir_entry(int port, int idx, char *name, uint32_t *size);
bool memcard_match(const char *pattern, const char *name);
bool memcard_format(int port);
bool memcard_rename(int port, const char *from, const char *to);
bool memcard_delete(int port, const char *name);
void memcard_sector_io(int port, int sector, uint8_t *buf, bool write);
