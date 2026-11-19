#include "locomotion.h"
#include <assert.h>
bool g_locomotion_unclamp;
int main(void) {
    g_locomotion_unclamp=false;
    assert(locomotion_instruction(0x8004faa8,0x28820800)==0x28820800);
    g_locomotion_unclamp=true;
    assert(locomotion_instruction(0x8004faa8,0x28820800)==0x28827fff);
    assert(locomotion_instruction(0x0004faa8,0x28820800)==0x28827fff);
    assert(locomotion_instruction(0x8004faa4,0x28820800)==0x28820800);
    assert(locomotion_instruction(0x8004faa8,0x288207ff)==0x288207ff);
    return 0;
}
