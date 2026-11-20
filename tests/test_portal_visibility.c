#include "portal_visibility.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
void bus_write16(uint32_t a,uint32_t v){(void)a;(void)v;abort();}

static CPU fixture(unsigned count) {
    memset(g_ram,0,sizeof g_ram);CPU c={0};
    c.r[4]=0;c.r[5]=0x80004000;c.r[6]=2;c.r[28]=0x80002000;c.r[31]=0x80035c14;
    mem_w32(0x35c0c,0x0c00d458);mem_w32(0x35c10,0x24060002);
    mem_w32(0x2000+484,0x80005000);mem_w16(0x5000+50,count);
    mem_w32(0x5000+32,0x80006000);mem_w32(0x5000+28,0x80008000);
    for(unsigned i=0;i<count && i<64;i++) {
        mem_w16(0x6000+40*i+20,2*i);mem_w16(0x6000+40*i+22,2);
    }
    return c;
}
int main(void) {
    CPU c=fixture(17),before=c;assert(portal_visibility_depth(&c)==17);
    assert(!memcmp(&c,&before,sizeof c));
    c.r[31]=0x35c14;assert(portal_visibility_depth(&c)==17);
    /* A chain with three consecutive portals now reaches its fourth room.
     * Recursive calls retain the decrement, including zero used by barriers. */
    for(unsigned n=1;n<=64;n++) {
        c=fixture(n);unsigned depth=portal_visibility_depth(&c);assert(depth==(n>2?n:2));
        c.r[31]=0x80035660;
        for(unsigned remaining=0;remaining<=depth;remaining++) {
            c.r[6]=remaining;assert(portal_visibility_depth(&c)==remaining);
        }
    }
    c=fixture(17);c.r[4]=17;assert(portal_visibility_depth(&c)==2);
    c=fixture(0);assert(portal_visibility_depth(&c)==2);
    c=fixture(65);assert(portal_visibility_depth(&c)==2);
    c=fixture(17);c.r[5]=0x801ffff8;assert(portal_visibility_depth(&c)==2);
    c=fixture(17);c.r[28]=0x801fff00;assert(portal_visibility_depth(&c)==2);
    c=fixture(17);mem_w32(0x5000+32,0x1ffff0);assert(portal_visibility_depth(&c)==2);
    c=fixture(17);mem_w32(0x5000+28,0x1ffff0);assert(portal_visibility_depth(&c)==2);
    c=fixture(17);mem_w16(0x6000+20,65535);mem_w16(0x6000+22,65535);
    assert(portal_visibility_depth(&c)==2);
    c=fixture(17);mem_w32(0x35c10,0x24060003);assert(portal_visibility_depth(&c)==2);
    c=fixture(17);c.r[6]=0;assert(portal_visibility_depth(&c)==0);
    puts("Portal traversal: resident-room budget, recursive/barrier budgets, 64-bit visited capacity and overlay guards passed.");
    return 0;
}
