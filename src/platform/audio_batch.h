#pragma once
#include <stdint.h>
#include <string.h>
enum { AUDIO_BATCH_FRAMES=256 };
typedef struct { int16_t samples[AUDIO_BATCH_FRAMES*2];int frames; } AudioBatch;
typedef void (*AudioBatchSubmit)(const int16_t *samples,int frames,void *context);
/* Coalesce the SPU's short timed writes; preserve every generated sample
 * and its order. At 44.1 kHz the staging delay is less than six ms. */
static inline void audio_batch_push(AudioBatch *b,const int16_t *samples,int frames,
                                   AudioBatchSubmit submit,void *context) {
    while(frames>0) {
        int n=AUDIO_BATCH_FRAMES-b->frames;if(n>frames)n=frames;
        memcpy(b->samples+b->frames*2,samples,(size_t)n*4);
        b->frames+=n;samples+=n*2;frames-=n;
        if(b->frames==AUDIO_BATCH_FRAMES) {
            submit(b->samples,b->frames,context);b->frames=0;
        }
    }
}
