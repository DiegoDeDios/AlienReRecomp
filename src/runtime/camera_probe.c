#include "camera_probe.h"
#include "pgxp.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>

bool camera_probe_run(CPU *c,uint32_t point,uint32_t delta,const double start[3],
                      const double movement[3],void (*sweep)(CPU*),uint32_t result[3]) {
    static uint8_t *ram;
    static bool active;
    uint32_t p=point&0x1fffffffu,d=delta&0x1fffffffu;
    if(active || p>sizeof g_ram-16 || d>sizeof g_ram-12 || !sweep)return false;
    int32_t position[3],step[3];
    for(int i=0;i<3;i++) {
        if(!isfinite(start[i]) || !isfinite(movement[i]) ||
           start[i]<INT_MIN+.5 || start[i]>INT_MAX-.5 ||
           movement[i]<INT_MIN+.5 || movement[i]>INT_MAX-.5)return false;
        position[i]=(int32_t)llround(start[i]);step[i]=(int32_t)llround(movement[i]);
    }
    if(!ram)ram=malloc(sizeof g_ram);
    if(!ram)return false;
    active=true;CPU saved=*c;uint8_t scratch[sizeof g_scratch];
    memcpy(ram,g_ram,sizeof g_ram);memcpy(scratch,g_scratch,sizeof g_scratch);
    bool pgxp=g_pgxp;
    /* Queries touch only guest RAM, scratch and CPU/GTE. Prevent event service
     * and precision/diagnostic side effects during this read-only replay. */
    g_pgxp=false;c->next_event=INT64_MAX;
    for(int i=0;i<3;i++){mem_w32(point+4*i,(uint32_t)position[i]);mem_w32(delta+4*i,(uint32_t)step[i]);}
    sweep(c);
    for(int i=0;i<3;i++)result[i]=mem_r32(point+4*i);
    memcpy(g_ram,ram,sizeof g_ram);memcpy(g_scratch,scratch,sizeof g_scratch);*c=saved;
    g_pgxp=pgxp;active=false;
    return true;
}
