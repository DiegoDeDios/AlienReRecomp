#include "widescreen.h"
#include "psx.h"
#define ram g_ram
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} }while(0)

static void word(unsigned at,uint32_t w){memcpy(ram+at,&w,4);}
int main(void) {
    /* Minimal code identity fixture, no executable or movie data. */
    word(0x202A0,0x0C00DC53);word(0x202A4,0x24060001);
    word(0x202C4,0xA02047A8);word(0x3716C,0x14C00015);
    word(0x52F8C,0x24A50004);
    ram[0xA47A8]=1;ram[0xA87B4]=4;
    CHECK(!ws_autodoc_scene(NULL,sizeof ram));
    CHECK(!ws_autodoc_scene(ram,0xA87B4));
    for(int clip=0;clip<256;clip++) {
        ram[0xA87B4]=(uint8_t)clip;
        CHECK(ws_autodoc_scene(ram,sizeof ram)==(clip>=4 && clip<=7));
    }
    ram[0xA87B4]=4;
    for(int pending=0;pending<3;pending++) {
        ram[0xA47A8]=(uint8_t)pending;
        CHECK(ws_autodoc_scene(ram,sizeof ram)==(pending==1));
    }
    ram[0xA47A8]=1;
    const unsigned sites[]={0x202A0,0x202A4,0x202C4,0x3716C,0x52F8C};
    for(unsigned i=0;i<sizeof sites/sizeof sites[0];i++) {
        ram[sites[i]]^=1;CHECK(!ws_autodoc_scene(ram,sizeof ram));ram[sites[i]]^=1;
    }
    const float aspects[]={4.0f/3,16.0f/9,21.0f/9,32.0f/9};
    for(unsigned a=0;a<sizeof aspects/sizeof aspects[0];a++) {
        ws_configure(aspects[a]);g_ws_3d_count=1;ws_new_frame();
        bool wide=a!=0;CHECK(g_ws_wide_display==wide);
        float k=g_ws_k;
        /* A 15-second retained scene must not become a 2D-only screen
         * merely because no new polygons were projected for twelve frames. */
        for(int frame=0;frame<900;frame++) {
            ws_new_frame();
            CHECK(g_ws_wide_display==wide && g_ws_3d_count==0 && g_ws_k==k);
        }
        /* Ordinary 2D/loading screens still fall back, and an old save
         * taken after that fallback recovers as soon as its guest state loads. */
        ram[0xA47A8]=0;
        for(int frame=0;frame<12;frame++)ws_new_frame();
        CHECK(!g_ws_wide_display);
        ram[0xA47A8]=1;ws_new_frame();
        CHECK(g_ws_wide_display==wide);
        ram[0xA47A8]=0;
        for(int frame=0;frame<12;frame++)ws_new_frame();
        CHECK(!g_ws_wide_display);
        g_ws_3d_count=1;ws_new_frame();CHECK(g_ws_wide_display==wide);
        ram[0xA47A8]=1;
    }
    puts("Widescreen retained scene: all autodoc clips, four aspects, exit, resume and 2D fallback passed.");
    return 0;
}
