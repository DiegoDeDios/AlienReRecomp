/* GNU/MinGW: -O2 -flto -fwhole-program retains the actual ADSR routine
 * without the unreferenced host audio, scheduler or emulator boot paths. */
#include "../src/hw/spu.c"
#include <assert.h>

int main(void) {
    Voice v = {0};
    v.phase = ADSR_RELEASE;
    v.adsr_hi = 12;
    v.adsr_level = 1000;
    v.adsr_wait = 1000000;
    adsr_step(&v);
    assert(v.adsr_wait == 0 && v.adsr_level == 1000);
    adsr_step(&v);
    assert(v.adsr_level == 992 && v.adsr_wait == 1);

    v.adsr_wait = 1;
    adsr_step(&v);
    assert(v.adsr_level == 992 && v.adsr_wait == 0);

    const AdsrPhase phases[] = {ADSR_ATTACK, ADSR_SUSTAIN, ADSR_RELEASE};
    for (unsigned i = 0; i < sizeof phases / sizeof phases[0]; ++i) {
        v.phase = phases[i];
        v.adsr_lo = 0x7f00;
        v.adsr_hi = 0x1fdf;
        v.adsr_level = 1234;
        v.adsr_wait = 1000000;
        adsr_step(&v);
        assert(v.adsr_level == 1234 && v.adsr_wait == 1000000);
    }

    v.phase = ADSR_RELEASE;
    v.adsr_hi = 0;
    v.adsr_level = 100;
    v.adsr_wait = 0;
    adsr_step(&v);
    assert(v.phase == ADSR_OFF && v.adsr_level == 0);
    puts("ADSR: stale and valid waits, hardware holds, release completion passed.");
    return 0;
}
