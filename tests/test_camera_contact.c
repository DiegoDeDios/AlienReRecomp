#include "camera_contact.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static double dot(const double a[3],const double b[3]) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
int main(void) {
    CameraContacts c={0};double start[3]={9683555.375,5267503.125,24462190.5};
    double p[3]={9695232.75,5390000.5,24491008.125},out[3]={9,8,7},before[3];memcpy(before,out,sizeof out);
    CHECK(!camera_contact_clip(&c,start,p,out) && !memcmp(before,out,sizeof out));
    int32_t ceiling[3]={0,-4096,0};camera_contact_add(&c,ceiling,5382144);
    /* Eye segment clipped at the ceiling, in both tangent directions and
     * with several eye heights (standing/crouching). Fraction remains exact. */
    for(int i=-1000;i<=1000;i++)for(int height=0;height<2;height++) {
        p[0]=start[0]+i*817.125;p[1]=start[1]+(height?95.:28.)*4096;p[2]=start[2]-i*421.75;
        double t=(5382016-start[1])/(p[1]-start[1]);if(t>1)t=1;
        CHECK(camera_contact_clip(&c,start,p,out));
        for(int k=0;k<3;k++)CHECK(fabs(out[k]-(start[k]+t*(p[k]-start[k])))<1e-7);
        CHECK(out[1]<=5382016+1e-7);
    }
    p[1]=5300000;CHECK(camera_contact_clip(&c,start,p,out));
    for(int k=0;k<3;k++)CHECK(fabs(out[k]-p[k])<1e-7);
    /* Sloping contacts on every axis, both orientations and fractional
     * distances. Check an independently calculated segment intersection. */
    for(int axis=0;axis<3;axis++)for(int slope=-1800;slope<=1800;slope+=120) {
        c=(CameraContacts){0};int32_t n[3]={0};n[axis]=3900;n[(axis+1)%3]=slope;
        camera_contact_add(&c,n,1234567);double normal[3];
        for(int k=0;k<3;k++){normal[k]=n[k]/4096.;start[k]=0;p[k]=-normal[k]*3000000;}
        double a=1234567-128,b=a+dot(normal,p),t=a/(a-b);
        CHECK(camera_contact_clip(&c,start,p,out));
        for(int k=0;k<3;k++)CHECK(fabs(out[k]-t*p[k])<1e-7);
        CHECK(fabs(dot(normal,out)+1234567-128)<1e-7);
    }
    /* Multiple contacts, independent of insertion order, including a
     * redundant plane. Camera must stay on its ray and inside every plane. */
    int32_t axes[3][3]={{4096,0,0},{0,4096,0},{0,0,4096}};
    for(int order=0;order<3;order++) {
        c=(CameraContacts){0};for(int i=0;i<3;i++)camera_contact_add(&c,axes[(i+order)%3],0);
        camera_contact_add(&c,axes[0],100);camera_contact_add(&c,axes[0],100);CHECK(c.count==4);
        double origin[3]={1024,2048,4096},desired[3]={-8192.5,-4096.75,-1.125},t=1;
        for(int k=0;k<3;k++){double f=(origin[k]-128)/(origin[k]-desired[k]);if(f<t)t=f;}
        CHECK(camera_contact_clip(&c,origin,desired,out));
        for(int k=0;k<3;k++){CHECK(fabs(out[k]-(origin[k]+t*(desired[k]-origin[k])))<1e-7);CHECK(out[k]>=128-1e-7);}
    }
    /* Invalid/penetrating starts and malformed/overflowing contact sets
     * fail without modifying the caller's output. */
    c=(CameraContacts){0};camera_contact_add(&c,axes[0],0);double bad[3]={0};memcpy(before,out,sizeof out);
    CHECK(!camera_contact_clip(&c,bad,p,out) && !memcmp(before,out,sizeof out));
    bad[0]=NAN;CHECK(!camera_contact_clip(&c,bad,p,out));
    c=(CameraContacts){0};int32_t zero[3]={0};camera_contact_add(&c,zero,0);CHECK(!camera_contact_clip(&c,start,p,out));
    c=(CameraContacts){0};for(int i=0;i<5;i++)camera_contact_add(&c,axes[0],i);CHECK(!camera_contact_clip(&c,start,p,out));
    puts("Camera contacts: precise ceiling/slopes, standing/crouching eye segments, corners and safe fallbacks passed.");return 0;
}
