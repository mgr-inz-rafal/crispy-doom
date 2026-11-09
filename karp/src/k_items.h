//
// DESCRIPTION:
//	Karp items (karp/docs/plan.md, 7.5): every item lying in the level -
//	health, armour, ammo, weapons, power-ups; keys are the brain's - and
//	what picking each up would be worth now, as a detour the player may
//	make for it.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_ITEMS__
#define __K_ITEMS__

#include "k_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    K_ITEM_HEALTH,
    K_ITEM_ARMOUR,
    K_ITEM_AMMO,
    K_ITEM_WEAPON,          // a backpack too
    K_ITEM_POWER,           // power-ups
} k_itemkind_t;

typedef struct
{
    k_vec2_t at;
    const char *name;       // "medikit"
    k_itemkind_t kind;
    int present;            // still lying there
    int dropped;            // dropped by a monster (half the ammo)
    int reachable;          // some spot the player can stand at touches it
                            // (K_Graph_PickupSpot): not under a tree, say
    k_vec2_t spot;          // reachable: where to go to pick it up
    double worth;           // what picking it up would give now, roughly in
                            // health points; 0 if the player would not pick
                            // it up, or it would give nothing that matters
    double together;        // with the items beside it, picked up on the same
                            // trip
    double danger;          // what the monsters guarding it would cost in
                            // health (K_Combat_Danger), beyond those that see
                            // the player where it stands (fought anyway); a
                            // quarter for a weapon not held while nothing
                            // better than the pistol is. The guards,
                            // reachable and spot looked at about once a
                            // second while it is worth anything
    double reach;           // how far out of its way the player may go for
                            // it and those beside it now, the danger taken
                            // off, in map units of detour (0: none, or out
                            // of reach)
    int urgent;             // worth breaking a fight off for, close by:
                            // health when LOW or CRITICAL (a stimpack's worth
                            // at least, with those beside it), a weapon the
                            // player lacks, and the ammo for a strong weapon
                            // while firepower is LOW or CRITICAL (any, while
                            // ammo is LOW), from ammo, a weapon or a backpack
    int escape;             // urgent, and worth leaving a fight for however
                            // far: the health, when LOW or CRITICAL, and the
                            // ammo when ammo is LOW or firepower CRITICAL
    int first;              // a weapon better than the pistol while nothing
                            // better is held (a pistol start), or one not
                            // held that beats a monster the player is
                            // outgunned by (beats): goes before other
                            // items, but for health when CRITICAL (E3M8:
                            // the plasma rifle passed for the rockets
                            // nearer, one at a time, all over the map)
    int suit;               // a radiation suit: the brain also weighs it by
                            // the floors it would keep out on the way on
    int beats;              // worth anything, it gives what would kill a
                            // monster the player met that what it holds
                            // cannot (K_Combat_Beats): running out
} k_item_t;

// Called by Karp on every level load.
void K_Items_Reset(void);

// Once a tic in a level, after the condition and combat: which items still
// lie where, and their worth now. Items keep their index for the level; one
// dropped later (a monster's clip) gets a new one.
void K_Items_Tick(void);

int K_Items_Count(void);
const k_item_t *K_Items_Get(int index);

// What the item a monster of the engine's mobjtype_t drops when it dies
// would be worth now: a zombieman's or SS's clip, a shotgun guy's shotgun,
// a chaingunner's chaingun; 0 for the rest.
double K_Items_DropWorth(int type);

#ifdef __cplusplus
}
#endif

#endif
