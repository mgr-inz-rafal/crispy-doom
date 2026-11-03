//
// DESCRIPTION:
//	Karp combat (karp/docs/plan.md, 7.2-7.4): what the bot sees - every
//	monster, and the projectiles in flight - which monster it shoots at,
//	with which weapon, the aiming and firing, and getting out of the way
//	of projectiles.
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
    int threats;            // projectiles and shots wound up for that could
                            // reach the player (k_threat.h)
    int dodges;             // stepped out of their way out of a fight, this
                            // level
    int missile_hits;       // hits from projectiles this level, and the
    int missile_damage;     // health they took
    int dodgeable_hits;     // of those, seen in time to step out of their
    int dodgeable_damage;   // way
    int foreseen_hits;      // and of those, taken knowingly: every way the
                            // player could have taken cost more
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
// be then (K_Nav_Predict); until a shot pressed for leaves the weapon, what
// it was fired at. Returns 0 when there is nothing to aim at.
int K_Combat_Aim(int tic, double *angle);

// In battle mode, or stepping out of the way of something (a blast, blows,
// a projectile): the velocity to move at (map units per tic), nav holding
// its route - or following it still, for a sidestep on the way. Returns 0
// when nav drives.
int K_Combat_Move(double *vx, double *vy);

// Logs what projectiles took from the player this level, and how often it
// stepped out of their way.
void K_Combat_LogSummary(void);

// What going to (x, y), on a floor at `floor`, would cost in health,
// roughly: the live monsters within 1024 that could see the player there
// (in full within 512, less beyond), each by what a fight with its kind
// takes; asleep and facing away, half. Only the monsters at the spot, not
// those on the way to it (the user: those are met and fought anyway, and
// move about).
double K_Combat_Danger(double x, double y, double floor);

// How many monsters the player can see within reach of it.
int K_Combat_MonstersWithin(double reach);

// Set by the brain while it goes for health with health CRITICAL, or dashes
// for a weapon or ammo close by, with why ("health CRITICAL, going for
// health"); NULL when not: no fight is started, one going on is broken off,
// and the player runs, shooting on the way (plan, 7.4: fleeing).
void K_Combat_Escape(const char *why);

// Set by the brain while it wants the barrel at (x, y) out of the way: shot
// at from where the player is, once no monster is to be shot, with a weapon
// that pops it (brought up if need be). It must be in reach of the shots,
// and its chain clear of the player.
void K_Combat_ShootBarrel(int on, double x, double y);

// How many barrels within reach of (x, y) are shot and about to blow up.
int K_Combat_BlastsPending(double x, double y, double reach);

// After nav built the command: brings up the weapon wanted, and fires if,
// after the command's turn, the player faces the target, and the shot is
// safe both from there and from where it leaves the weapon. Leaves alone a
// command in which nav uses, fires or changes weapons itself.
void K_Combat_Fire(int tic, k_command_t *command);

// What a kind of monster (a mobjtype_t) is called: "baron of hell".
const char *K_Combat_KindName(int type);

int K_Combat_MonsterCount(void);
const k_monster_t *K_Combat_Monster(int index);
int K_Combat_ProjectileCount(void);
const k_projectile_t *K_Combat_Projectile(int index);

const k_combatstatus_t *K_Combat_GetStatus(void);

#ifdef __cplusplus
}
#endif

#endif
