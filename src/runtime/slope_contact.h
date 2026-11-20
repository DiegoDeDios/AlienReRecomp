#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Signed 20.12 positions/deltas, 4.12 inward plane normal and 20.12 D. */
bool slope_contact_step(const int32_t start[3],const int32_t movement[3],
                        const int32_t normal[3],int32_t distance,int32_t result[3]);
