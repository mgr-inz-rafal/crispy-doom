//
// DESCRIPTION:
//	Karp combat (karp/docs/plan.md, 7.2-7.4): what the bot sees - every
//	monster, and the projectiles in flight - which monster it shoots at,
//	with which weapon, and the aiming and firing.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_COMBAT__
#define __K_COMBAT__

#include "k_map.h"
#include "k_nav.h"

#ifdef __cplusplus
extern "C" {
#endif

// A monster alive in the level. The bot knows them all, as a player who
// knows the map would (the user); it shoots only at those it can see.
typedef struct
{
    int type;               // the engine's mobjtype_t
    const char *name;       // "imp"
    k_vec2_t at;
    double angle;           // the way it faces, radians
    double radius;
    int health;
    int awake;              // it has a target: it saw or heard the player,
                            // or fights another monster
    int attacking;          // winding up an attack or making it
    int hitscan;            // its attack is a bullet, which cannot be dodged
    int visible;            // the player can see it
    int shootable;          // the player's shots would find it (K_Sight_Aim):
                            // more than seeing a row of its pixels
    double z, height;       // its feet, and how tall it is
    double distance;        // from the player
} k_monster_t;

typedef struct
{
    const char *name;       // "imp fireball"
    k_vec2_t at;
    k_vec2_t velocity;      // map units per tic
    double radius;
    int own;                // the player fired it
    int incoming;           // it will pass within reach of the player
    double eta;             // incoming: tics to its closest approach
} k_projectile_t;

typedef struct
{
    int target;             // index into the monsters, -1 if none
    const char *weapon;     // the weapon wanted, NULL if none
    int alive;              // monsters alive in the level
    int visible;            // of those, seen by the player
    int awake;              // of those seen, awake
    int kills;              // this level, as the level counts them
    int shots;              // fire presses this level
    int died;               // the player died this level
    int battle;             // fighting: the route waits (battle mode)
    int barrel;             // shooting a barrel, at barrel_at
    k_vec2_t barrel_at;
    char text[96];          // what it is doing, for the console
} k_combatstatus_t;

// Called by Karp on every level load.
void K_Combat_Reset(void);

// Once a tic in a level, whoever drives: the monsters and projectiles, what
// the player can see; while the bot drives, the target and the weapon for
// it. Logs the hits the player takes, the monsters that die and its death.
void K_Combat_Tick(int driving);

// For the command that will run as game tic `tic`, built while the bot
// drives: the way to face to shoot the target, from where the player will
// be then (K_Nav_Predict). Returns 0 when there is nothing to aim at.
int K_Combat_Aim(int tic, double *angle);

// In battle mode: the velocity to move at (map units per tic), nav holding
// its route. Returns 0 when not fighting: nav drives.
int K_Combat_Move(double *vx, double *vy);

// What going to (x, y), on a floor at `floor`, would cost in health,
// roughly: the live monsters within 1024 that could see the player there
// (in full within 512, less beyond), each by what a fight with its kind
// takes; asleep and facing away, half. Only the monsters at the spot, not
// those on the way to it (the user: those are met and fought anyway, and
// move about).
double K_Combat_Danger(double x, double y, double floor);

// Set by the brain while it goes for health with health CRITICAL: no fight
// is started, one going on is broken off, and the player runs, shooting on
// the way (plan, 7.4: fleeing).
void K_Combat_Escape(int on);

// Set by the brain while it wants the barrel at (x, y) out of the way: shot
// at from where the player is, once no monster is to be shot, with a weapon
// that pops it (brought up if need be). It must be in reach of the shots,
// and its chain clear of the player.
void K_Combat_ShootBarrel(int on, double x, double y);

// How many barrels within reach of (x, y) are shot and about to blow up.
int K_Combat_BlastsPending(double x, double y, double reach);

// After nav built the command: brings up the weapon wanted, and fires if,
// after the command's turn, the player faces the target. Leaves alone a
// command in which nav uses, fires or changes weapons itself.
void K_Combat_Fire(int tic, k_command_t *command);

int K_Combat_MonsterCount(void);
const k_monster_t *K_Combat_Monster(int index);
int K_Combat_ProjectileCount(void);
const k_projectile_t *K_Combat_Projectile(int index);

const k_combatstatus_t *K_Combat_GetStatus(void);

#ifdef __cplusplus
}
#endif

#endif
