#include "slope_contact.h"
#include <limits.h>

bool slope_contact_step(const int32_t start[3],const int32_t movement[3],
                        const int32_t normal[3],int32_t distance,int32_t result[3]) {
    /* Upward, approximately unit, walkable support only (at most 45 degrees).
     * Keep flat floors, walls, ceilings and invalid planes on the guest path. */
    if(normal[1]<2897 || normal[1]>4096 || !(normal[0] || normal[2]))return false;
    int64_t length2=0,endpoint[3];
    for(int i=0;i<3;i++) {
        if(normal[i]<-4096 || normal[i]>4096)return false;
        length2+=(int64_t)normal[i]*normal[i];
        endpoint[i]=(int64_t)start[i]+movement[i];
        if(endpoint[i]<INT32_MIN || endpoint[i]>INT32_MAX)return false;
    }
    if(length2<15938355 || length2>17616077 ||
       length2>2*(int64_t)normal[1]*normal[1])return false; /* unit +/- 5% squared */
    /* Solve N dot endpoint + D >= 128 by lifting Y, not sliding the entire
     * gravity/snap vector along N. Round inward so a fixed-point remainder
     * cannot alternate sides of the support plane on successive updates. */
    int64_t numerator=((int64_t)128-distance)*4096-
        (int64_t)normal[0]*endpoint[0]-(int64_t)normal[2]*endpoint[2];
    int64_t y=numerator/normal[1];
    if(numerator%normal[1]>0)y++;
    if(y<endpoint[1])y=endpoint[1];
    int64_t dy=y-start[1];
    if(y<INT32_MIN || y>INT32_MAX || dy<INT32_MIN || dy>INT32_MAX)return false;
    result[0]=movement[0];result[1]=(int32_t)dy;result[2]=movement[2];
    return true;
}
