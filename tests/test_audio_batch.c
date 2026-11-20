#include "audio_batch.h"
#include <stdio.h>
static int16_t output[8192*2];static int count,calls;
static void submit(const int16_t *p,int frames,void *context) {
    (void)context;memcpy(output+count*2,p,(size_t)frames*4);count+=frames;calls++;
}
int main(void) {
    int16_t input[8192*2];AudioBatch batch={0};
    for(int i=0;i<8192*2;i++)input[i]=(int16_t)(i*977+32767);
    int at=0;
    while(at<8192) {
        int frames=(at*17)%97+1;if(frames>8192-at)frames=8192-at;
        audio_batch_push(&batch,input+at*2,frames,submit,NULL);at+=frames;
    }
    if(count!=8192 || batch.frames || calls!=32 || memcmp(input,output,sizeof input))return 1;
    puts("SPU burst batching preserves all stereo samples across irregular writes.");return 0;
}
