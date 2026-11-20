#include "camera_contact.h"
#include <math.h>
#include <string.h>

void camera_contact_add(CameraContacts *c,const int32_t normal[3],int32_t distance) {
    double n[3],norm=0;
    for(int i=0;i<3;i++){n[i]=normal[i]/4096.0;norm+=n[i]*n[i];}
    if(norm<0.25 || norm>4.0){c->invalid=true;return;}
    for(unsigned j=0;j<c->count;j++)
        if(c->distance[j]==distance && !memcmp(c->normal[j],n,sizeof n))return;
    if(c->count==4){c->invalid=true;return;}
    memcpy(c->normal[c->count],n,sizeof n);c->distance[c->count++]=distance;
}

/* Clip the precise eye segment against the contacts selected by the guest.
 * A camera stops at the first surface instead of inheriting the integer
 * body's slide/step tangential overtravel. The same-frame ray remains
 * anchored to the player; no previous frame or smoothing state is used. */
bool camera_contact_clip(const CameraContacts *c,const double start[3],const double desired[3],double out[3]) {
    if(c->invalid || !c->count || c->count>4)return false;
    double fraction=1;
    for(int k=0;k<3;k++)if(!isfinite(start[k]) || !isfinite(desired[k]))return false;
    for(unsigned i=0;i<c->count;i++) {
        double a=c->distance[i]-128.0,b=a;
        for(int k=0;k<3;k++){a+=c->normal[i][k]*start[k];b+=c->normal[i][k]*desired[k];}
        if(a<0)return false; /* retain the original result for penetrating starts */
        if(b<0){double t=a/(a-b);if(t<fraction)fraction=t;}
    }
    for(int k=0;k<3;k++)out[k]=start[k]+fraction*(desired[k]-start[k]);
    return true;
}
