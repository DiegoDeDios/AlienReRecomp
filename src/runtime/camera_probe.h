#pragma once
#include "psx.h"
/* Replay a single point sweep with precise 20.12 endpoints, without committing
 * any guest state or advancing hardware time. Used only by the player view. */
bool camera_probe_run(CPU *cpu,uint32_t point,uint32_t delta,const double start[3],
                      const double movement[3],void (*sweep)(CPU*),uint32_t result[3]);
