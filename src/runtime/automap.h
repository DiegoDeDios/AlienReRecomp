/*
 * Automap: the walls of the current level section as 2D lines, sliced from
 * the collision BSP the game keeps in RAM, for the rooms the player has been in.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* World units: x to the right of the map, z down it. */
typedef struct AutomapSeg {
    float x0, z0, x1, z1;
} AutomapSeg;

typedef struct AutomapView {
    const AutomapSeg *segs;
    int nsegs;
    float x, z;  /* player */
} AutomapView;

/* the game drew its HUD this frame: the player is in play */
void automap_game_frame(void);
/* false unless the player has been in play within the last half second */
bool automap_view(AutomapView *view);

#ifdef __cplusplus
}
#endif
