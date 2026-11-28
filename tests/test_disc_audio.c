#include "psx.h"
#include <stdarg.h>
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int g_log_level;
void log_msg(int level,const char *sys,const char *fmt,...) {(void)level;(void)sys;(void)fmt;}
int main(void) {
    FILE *f=fopen("disc-audio-test.bin","wb");CHECK(f);uint8_t sector[2352]={0};
    for(int i=0;i<6;i++)CHECK(fwrite(sector,1,sizeof sector,f)==sizeof sector);fclose(f);
    f=fopen("disc-audio-test.cue","w");CHECK(f);
    fputs("FILE \"disc-audio-test.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 01 00:00:03\n",f);fclose(f);
    CHECK(disc_open("disc-audio-test.cue"));
    CHECK(!disc_sector_is_audio(0)&&!disc_sector_is_audio(2));
    CHECK(disc_sector_is_audio(3)&&disc_sector_is_audio(5)&&!disc_sector_is_audio(6));
    CHECK(disc_read_sector(3,sector));disc_close();CHECK(!disc_sector_is_audio(0));
    remove("disc-audio-test.cue");remove("disc-audio-test.bin");
    puts("CD audio: data and pre-music sectors stay silent; audio boundaries and file access passed.");return 0;
}
