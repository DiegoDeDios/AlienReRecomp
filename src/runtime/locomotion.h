#pragma once
#include <stdbool.h>
#include <stdint.h>
extern bool g_locomotion_unclamp;
static inline uint32_t locomotion_instruction(uint32_t pc, uint32_t instruction) {
    if ((pc | 0x80000000u) == 0x8004FAA8u &&
        instruction == 0x28820800u && g_locomotion_unclamp)
        return 0x28827FFFu;
    return instruction;
}
