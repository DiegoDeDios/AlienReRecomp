#include "widescreen.h"
#include "psx.h"
#define ram g_ram
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static void word(unsigned a,uint32_t v){memcpy(ram+a,&v,4);}
static void half(unsigned a,uint16_t v){memcpy(ram+a,&v,2);}
static void fixture(void) {
    memset(ram,0,sizeof ram);
    word(0x976C0,0x47EFC);word(0x47F40,0x0C00B721);
    word(0x47F48,0x0C00B6C3);word(0x4095C,0x0040F809);
    word(0xA882C,0x5000);word(0xA8824,0x6000);half(0xA8834,103);
    half(0x6000,103);word(0x5000+88,0x6002);
}
int main(void) {
    fixture();CHECK(ws_script_message_scene(ram,sizeof ram));
    CHECK(ws_script_message_scene(ram,sizeof ram));
    CHECK(!ws_script_message_scene(NULL,sizeof ram));
    CHECK(!ws_script_message_scene(ram,0xA8835));
    /* A stale opcode after the VM returns must not widen a loading/movie UI. */
    word(0xA882C,0);CHECK(!ws_script_message_scene(ram,sizeof ram));
    fixture();word(0xA882C,0x801ffff0);CHECK(!ws_script_message_scene(ram,sizeof ram));
    fixture();word(0xA8824,0x801fffff);CHECK(!ws_script_message_scene(ram,sizeof ram));
    fixture();word(0x5000+88,0x6004);CHECK(!ws_script_message_scene(ram,sizeof ram));
    fixture();half(0x6000,104);CHECK(!ws_script_message_scene(ram,sizeof ram));
    fixture();half(0xA8834,104);CHECK(!ws_script_message_scene(ram,sizeof ram));
    const unsigned sites[]={0x976C0,0x47F40,0x47F48,0x4095C};
    for(unsigned i=0;i<sizeof sites/sizeof sites[0];i++) {
        fixture();ram[sites[i]]^=1;CHECK(!ws_script_message_scene(ram,sizeof ram));
    }
    fixture();half(0x6000,0x8067);word(0xA882C,0x80005000);word(0xA8824,0x80006000);
    word(0x5000+88,0x80006002);CHECK(ws_script_message_scene(ram,sizeof ram));
    const float aspects[]={4.0f/3,16.0f/9,21.0f/9,32.0f/9};
    for(unsigned i=0;i<4;i++) {
        fixture();ws_configure(aspects[i]);g_ws_3d_count=1;ws_new_frame();
        for(int f=0;f<900;f++) {
            ws_new_frame();CHECK(g_ws_wide_display==(i!=0));
        }
        word(0xA882C,0);
        for(int f=0;f<12;f++)ws_new_frame();
        CHECK(!g_ws_wide_display);
        /* Loading an old state already in the dialog recovers immediately. */
        word(0xA882C,0x5000);ws_new_frame();
        CHECK(g_ws_wide_display==(i!=0));
    }
    puts("Retained script scene: live VM, stale/invalid state, overlay guards, four aspects, long waits, exit and resume passed.");
    return 0;
}
