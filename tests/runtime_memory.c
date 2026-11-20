/* Minimal emulated memory for geometry unit tests; no game/disc code. */
#include "psx.h"
#include <stdlib.h>
uint8_t g_ram[0x200000], g_scratch[0x400];
static CPU test_cpu;
CPU *g_cpu = &test_cpu;
uint32_t g_watch_lo, g_watch_len;
int g_log_level;
uint32_t bus_read32(uint32_t addr) { return 0; }
uint32_t bus_read16(uint32_t addr) { return 0; }
void bus_write32(uint32_t addr, uint32_t v) { abort(); }
void mem_watch_hit(uint32_t addr, uint32_t val, int size) { abort(); }
void log_msg(int level, const char *sys, const char *fmt, ...) { }
