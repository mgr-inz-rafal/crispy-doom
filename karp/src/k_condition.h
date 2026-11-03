//
// DESCRIPTION:
//	Karp condition (karp/docs/plan.md, 7.1): how the player is doing -
//	its ammo, health and armour, each a need of its own - and the
//	condition they add up to.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_CONDITION__
#define __K_CONDITION__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    K_NEED_OK,
    K_NEED_LOW,
    K_NEED_CRITICAL,        // health only
} k_need_t;

// From the lowest priority to the top.
typedef enum
{
    K_CONDITION_COMFY,
    K_CONDITION_LOW_AMMO,
    K_CONDITION_LOW_HEALTH,
    K_CONDITION_CRITICAL,
} k_condition_t;

typedef struct
{
    k_need_t ammo;
    k_need_t firepower;     // LOW: no strong weapon held can fire, the
                            // pistol is the best the player has
    k_need_t health;
    k_need_t armour;
    k_condition_t condition;    // the worst of the needs; armour alone does
                                // not change it
    int health_points;
    int armour_points;
    int bullets, shells, rockets, cells;
} k_conditionstatus_t;

// Called by Karp on every level load.
void K_Condition_Reset(void);

// Once a tic in a level.
void K_Condition_Tick(void);

const k_conditionstatus_t *K_Condition_Get(void);

// "COMFY", "LOW AMMO"...; "OK", "LOW", "CRITICAL".
const char *K_Condition_Name(k_condition_t condition);
const char *K_Condition_NeedName(k_need_t need);

// The rounds of an ammo type (am_clip...) below which it is low.
int K_Condition_AmmoLow(int ammo);

// Whether a weapon (weapontype_t) is a strong one: the shotguns, the
// chaingun, the rocket launcher, the plasma rifle, the BFG - not the fist,
// the chainsaw or the pistol.
int K_Condition_Strong(int weapon);

#ifdef __cplusplus
}
#endif

#endif
