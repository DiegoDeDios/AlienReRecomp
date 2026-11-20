#include "camera_walk.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
void bus_write16(uint32_t a,uint32_t v){(void)a;(void)v;abort();}
int main(void) {
    CPU c={0};uint32_t p=0x5000;
    mem_w32(0xa49e8,p);mem_w32(p+72,(uint32_t)-32768);
    c.r[4]=p+60;c.r[29]=0x80100000;c.r[31]=0x80021ea0;
    mem_w32(0x100074,0x80062358);
    CPU before=c;
    assert(camera_walk_upright(&c,p));assert(!memcmp(&before,&c,sizeof c));
    g_ram[p+369]=1;assert(camera_walk_upright(&c,p));
    for(unsigned stance=2;stance<8;stance++){g_ram[p+369]=stance;assert(!camera_walk_upright(&c,p));}
    g_ram[p+369]=0;mem_w32(p+72,0);assert(!camera_walk_upright(&c,p));
    mem_w32(p+72,(uint32_t)-32768);
    c.r[31]=0x80021ea4;assert(!camera_walk_upright(&c,p));c=before;
    mem_w32(0x100074,0x800622b4);assert(!camera_walk_upright(&c,p));
    mem_w32(0x100074,0x80062358);
    assert(!camera_walk_upright(&c,0));assert(!camera_walk_upright(&c,p+128));
    c.r[4]++;assert(!camera_walk_upright(&c,p));c=before;
    c.r[29]=0x801ffff0;assert(!camera_walk_upright(&c,p));c=before;
    mem_w32(0xa49e8,p+128);assert(!camera_walk_upright(&c,p));
    puts("Walking eye root: standing/crouched support, exact pose scope, airborne/ladder/script exclusion and CPU identity passed.");
    return 0;
}
