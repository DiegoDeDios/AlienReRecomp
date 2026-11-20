#include "portal_visibility.h"

static bool range(uint32_t p,unsigned bytes) {
    return bytes<=sizeof g_ram && p>=4 && p<=sizeof g_ram-bytes;
}

/* USA 35870 seeds the visible-room list, then calls 35160 with a fixed two
 * portal budget. That budget hides a visible third room down a straight hall.
 * Let the existing portal/frustum tests finish traversing the resident map.
 * Its two visited words prevent cycles and duplicate list entries; its closed
 * portal flags and distance cutoff still terminate their respective paths. */
unsigned portal_visibility_depth(const CPU *c) {
    unsigned requested=c->r[6];
    if(requested!=2 || (c->r[31]&0x1fffffff)!=0x35c14 ||
       mem_r32(0x35c0c)!=0x0c00d458 || mem_r32(0x35c10)!=0x24060002)
        return requested;
    uint32_t gp=c->r[28]&0x1fffffff,rect=c->r[5]&0x1fffffff;
    if(!range(gp,488) || !range(rect,12))return requested;
    uint32_t map=mem_r32(gp+484)&0x1fffffff;
    if(!range(map,52))return requested;
    unsigned count=mem_r16(map+50),room=c->r[4];
    /* The native visited set has 64 bits. Never enlarge its storage, recurse
     * into an unrecognized overlay or infer additional/unloaded sectors. */
    if(!count || count>64 || room>=count)return requested;
    uint32_t rooms=mem_r32(map+32)&0x1fffffff;
    uint32_t portals=mem_r32(map+28)&0x1fffffff;
    if(!range(rooms,40*count))return requested;
    for(unsigned i=0;i<count;i++) {
        unsigned end=(unsigned)mem_r16(rooms+40*i+20)+mem_r16(rooms+40*i+22);
        if(end && !range(portals,32*end))return requested;
    }
    return count>requested?count:requested;
}
