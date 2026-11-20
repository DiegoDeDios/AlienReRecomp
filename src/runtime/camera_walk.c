#include "camera_walk.h"

/* The second pose pass in the first-person camera builder extracts the eye
 * position. Its body root inherits transient floor-bevel pitch/roll, although
 * the view direction already cancels that tilt. Keep this positional anchor
 * upright while walking/crouching; the actual body and animation remain native.
 * The exact nested call sites exclude model drawing, aim, ladders and scripts. */
bool camera_walk_upright(const CPU *c,uint32_t player) {
    uint32_t p=player&0x1fffffff,sp=c->r[29]&0x1fffffff;
    if(p<4 || p>sizeof g_ram-376 || (c->r[4]&0x1fffffff)!=p+60 ||
       p!=(mem_r32(0x800a49e8)&0x1fffffff) ||
       (c->r[31]&0x1fffffff)!=0x21ea0 || (sp&3) ||
       sp<4 || sp>sizeof g_ram-120 ||
       (mem_r32(sp+116)&0x1fffffff)!=0x62358)return false;
    return mem_r8(p+369)<=1 && (int32_t)mem_r32(p+72)==-32768;
}
