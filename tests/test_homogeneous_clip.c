#include "geometry_clip.h"
#include "pgxp.h"
#include "psx.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
int main(void) {
    GeometryVertex t[3]={{-100,10,0,0,0,0,0,0},{80,20,2,100,0,200,0,0},{20,80,2,0,100,0,200,0}};
    GeometryVertex out[GEOMETRY_CLIP_CAPACITY],other[GEOMETRY_CLIP_CAPACITY];
    int n=geometry_clip_triangle(t,-1000,-1000,1000,1000,out);assert(n==4);
    GeometryVertex rev[3]={t[1],t[0],{0,-100,2,0,100,0,200,0}};
    int m=geometry_clip_triangle(rev,-1000,-1000,1000,1000,other),matched=0;
    for(int i=0;i<n;i++)for(int j=0;j<m;j++)
        if(out[i].z==1 && out[i].u==50 && !memcmp(out+i,other+j,sizeof *out))matched++;
    assert(matched==1);
    srand(1481);
    for(int k=0;k<50000;k++) {
        for(int i=0;i<3;i++)t[i]=(GeometryVertex){(rand()%2000000-1000000)*1.,(rand()%2000000-1000000)*1.,(rand()%20000-10000)*1.,0,0,128,128,128};
        n=geometry_clip_triangle(t,0,0,368,240,out);assert(n<=8);
        for(int i=0;i<n;i++) {
            double x=out[i].hx/out[i].z,y=out[i].hy/out[i].z;
            assert(out[i].z>=1 && isfinite(x) && isfinite(y));
            assert(x>=-1e-7 && x<=368+1e-7 && y>=-1e-7 && y<=240+1e-7);
        }
    }
    t[0].hx=NAN;assert(!geometry_clip_triangle(t,0,0,368,240,out));
    for(int z=-1;z<=1;z++) {
        pgxp_reset();pgxp_gte_push(1e12,-2e12,z,123,true);mem_w32(0x100,123);pgxp_swc2(0x100,14);
        PgxpVtx p;assert(pgxp_lookup(0x100,123,&p) && p.w==z && p.x==1e12);
        assert(!pgxp_lookup(0x100,124,&p));
        pgxp_mfc2(2,14);g_cpu->r[2]=123;pgxp_lw(2,0x1f801000);
        mem_w32(0x104,123);pgxp_sw(0x104,2);assert(!pgxp_lookup(0x104,123,&p));
    }
    g_pgxp_weapon=true;pgxp_gte_push(1,1,100,123,true);pgxp_swc2(0x100,14);
    PgxpVtx weapon;assert(pgxp_lookup(0x100,123,&weapon) && weapon.space==2);
    g_pgxp_weapon=false;
    pgxp_gte_push(INFINITY,0,1,123,true);pgxp_swc2(0x100,14);PgxpVtx p;assert(!pgxp_lookup(0x100,123,&p));
    puts("Homogeneous clipping: 50000 triangles, eye-plane validity, shared edges, finite viewport bounds and stale checks passed.");return 0;
}
