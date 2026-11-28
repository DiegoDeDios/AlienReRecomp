#include "psx.h"
#include "transform.h"
#include <assert.h>
#include <math.h>
void bus_write16(uint32_t a,uint32_t v){(void)a;(void)v;}
int main(void) {
    for(int convention=0;convention<2;convention++)for(int angle=0;angle<100;angle++) {
        transform_reset();memset(g_ram,0x31,sizeof g_ram);
        unsigned char raw[18];memcpy(raw,g_ram+0x2000,18);
        transform_rotation(0x2000,angle*0.137,angle*17.3125,angle*-3.177,convention!=0);
        assert(!memcmp(raw,g_ram+0x2000,18));
        double m[9];for(int i=0;i<9;i++)m[i]=transform_memory16(0x2000+2*i)/4096.0;
        for(int i=0;i<3;i++)for(int j=0;j<3;j++) {
            double d=0;for(int k=0;k<3;k++)d+=m[i*3+k]*m[j*3+k];
            assert(fabs(d-(i==j))<1e-12);
        }
    }
    puts("Analytical rotation: both conventions retain orthonormal fractional matrices without changing guest RAM.");return 0;
}
