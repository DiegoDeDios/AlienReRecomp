/*
 * Enemy damage scaling (native.c): how much of an enemy attack's damage the
 * player takes, in percent of the original. Applied before the game's own
 * difficulty multiplier; the total is the "all enemies" value times the
 * value of the attacker's group.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DMG_ALIENS,         /* alien warriors */
    DMG_FACEHUGGERS,
    DMG_CHESTBURSTERS,
    DMG_QUEEN,
    DMG_NEWBORN,
    DMG_MARINES,        /* marines, soldiers and other humans */
    DMG_SENTRY_TANK,
    DMG_ACID,           /* acid blood */
    DMG_GROUP_COUNT
};

extern int g_enemy_damage_pct;                        /* all enemies */
extern int g_enemy_damage_group_pct[DMG_GROUP_COUNT]; /* per group */

#ifdef __cplusplus
}
#endif
