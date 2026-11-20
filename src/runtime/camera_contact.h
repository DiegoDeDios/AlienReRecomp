#pragma once
#include <stdbool.h>
#include <stdint.h>

/* One camera sweep has at most four selected contacts. Coordinates and
 * distances retain the guest's 20.12 scale; normals use its 4.12 scale. */
typedef struct {
    double normal[4][3],distance[4];
    unsigned count;
    bool invalid;
} CameraContacts;
void camera_contact_add(CameraContacts *contacts,const int32_t normal[3],int32_t distance);
bool camera_contact_clip(const CameraContacts *contacts,const double start[3],const double desired[3],double out[3]);
