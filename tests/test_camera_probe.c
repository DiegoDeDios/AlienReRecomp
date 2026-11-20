#include "camera_probe.h"
#include "pgxp.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
bool g_pgxp=true;
static bool callback_ok;
static uint32_t expected_point[3],expected_delta[3];
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void collision(CPU *c) {
    callback_ok=!g_pgxp && c->next_event==INT64_MAX;
    for(int i=0;i<3;i++) {
        callback_ok&=mem_r32(0x11000+4*i)==expected_point[i];
        callback_ok&=mem_r32(0x12000+4*i)==expected_delta[i];
        mem_w32(0x11000+4*i,expected_point[i]+expected_delta[i]);
    }
    uint32_t nested[3];double value[3]={0};
    callback_ok&=!camera_probe_run(c,0x11000,0x12000,value,value,collision,nested);
    mem_w32(0x1234,0xdeadbeef);mem_w32(0x1ffffc,0x12345678);mem_w32(0x1f800010,0x87654321);
    memset(c,0x5a,sizeof *c); /* includes all GTE, CP0, pending loads and timing */
}
int main(void) {
    for(unsigned i=0;i<sizeof g_ram;i++)g_ram[i]=(uint8_t)(i*13+(i>>9));
    for(unsigned i=0;i<sizeof g_scratch;i++)g_scratch[i]=(uint8_t)(i*17);
    uint8_t *ram=malloc(sizeof g_ram);CHECK(ram);memcpy(ram,g_ram,sizeof g_ram);
    uint8_t scratch[sizeof g_scratch];memcpy(scratch,g_scratch,sizeof scratch);
    CPU c;memset(&c,0x23,sizeof c);CPU before=c;g_cpu=&c;
    double start[3]={9683555.375,-5267503.75,24462190.125},step[3]={7581.25,102353.625,-3218.875};
    uint32_t result[3];
    for(int i=0;i<3;i++){expected_point[i]=(uint32_t)(int32_t)llround(start[i]);expected_delta[i]=(uint32_t)(int32_t)llround(step[i]);}
    CHECK(camera_probe_run(&c,0x80011000,0x80012000,start,step,collision,result));CHECK(callback_ok);
    for(int i=0;i<3;i++)CHECK(result[i]==expected_point[i]+expected_delta[i]);
    CHECK(!memcmp(&c,&before,sizeof c));CHECK(!memcmp(g_ram,ram,sizeof g_ram));CHECK(!memcmp(g_scratch,scratch,sizeof scratch));
    CHECK(g_pgxp);
    CHECK(!camera_probe_run(&c,0x1ffffc,0x12000,start,step,collision,result));
    start[0]=NAN;CHECK(!camera_probe_run(&c,0x11000,0x12000,start,step,collision,result));
    CHECK(!memcmp(&c,&before,sizeof c) && !memcmp(g_ram,ram,sizeof g_ram));free(ram);
    puts("Camera probe: precise inputs, blocked hardware time, no CPU/GTE/RAM/scratch/precision-mode side effects and reentry fallback passed.");return 0;
}
