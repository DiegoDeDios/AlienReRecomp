#include "slope_contact.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void ramp(int nx,int ny,int nz) {
    int32_t n[3]={nx,ny,nz},p[3]={0,0,0},d[3],o[3];
    /* Repeated ascent/descent at walking and half backward speeds. A large
     * downward adhesion must never consume or amplify horizontal input. */
    const int speed[]={11375,5689,-11375,-5689,1,-1,0};
    for(unsigned j=0;j<sizeof speed/sizeof *speed;j++) {
        p[0]=p[2]=0;p[1]=128*4096/ny+1;
        for(int step=0;step<200;step++) {
            d[0]=nx?speed[j]:0;d[1]=-32768;d[2]=nz?speed[j]:0;
            assert(slope_contact_step(p,d,n,0,o));
            assert(o[0]==d[0] && o[2]==d[2]);
            for(int i=0;i<3;i++)p[i]+=o[i];
            int64_t side=(int64_t)nx*p[0]+(int64_t)ny*p[1]+(int64_t)nz*p[2];
            assert(side>=128*4096 && side<128*4096+ny);
        }
    }
}
int main(void) {
    ramp(0,4047,628);ramp(0,4047,-628);
    ramp(628,4047,0);ramp(-628,4047,0);
    ramp(444,4047,444);ramp(-444,4047,444);
    ramp(2896,2897,0); /* nearly 45 degrees */
    int32_t p[3]={0,0,0},d[3]={34,-32086,-5689},n[3]={0,4047,628},o[3];
    assert(slope_contact_step(p,d,n,0,o));
    assert(o[0]==34 && o[2]==-5689 && o[1]>900); /* uphill, not ~100 */
    p[1]=4096;d[1]=500; /* already above the floor: never pull down */
    assert(slope_contact_step(p,d,n,0,o) && o[1]==d[1]);
    const int32_t reject[][3]={{0,4096,0},{0,-4047,628},{4096,0,0},
        {3000,2700,0},{0,8000,1},{INT_MAX,4096,1},{1,4096,INT_MIN},{0,3000,0}};
    for(unsigned i=0;i<sizeof reject/sizeof *reject;i++)
        assert(!slope_contact_step(p,d,reject[i],0,o));
    p[0]=INT_MAX;d[0]=1;assert(!slope_contact_step(p,d,n,0,o));
    p[0]=0;d[0]=0;p[1]=INT_MIN;d[1]=0;
    assert(!slope_contact_step(p,d,n,INT_MIN,o));
    puts("Grounded slopes preserve horizontal input and inward support, including reversed/diagonal ramps; other contacts fall back.");
}
