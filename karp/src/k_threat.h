//
// DESCRIPTION:
//	Karp threats (karp/docs/plan.md, 7.4): what could hit the player in the
//	next K_THREAT_TICS tics - the monsters' projectiles in flight, lost
//	souls charging, the shots monsters are winding up for - and what a way
//	the player could go would cost in health from them.
//
//	Plain C types only, except the calls that take the engine's mobjs
//	(only C files use them).
//

#ifndef __K_THREAT__
#define __K_THREAT__

#include "k_map.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mobj_s;

// How far ahead ways are judged: a rocket 640 units off, an imp's fireball
// 320.
#define K_THREAT_TICS 32

// The hit that costs most along a way.
typedef struct
{
    const char *name;       // "imp fireball", NULL if nothing hits
    int tic;                // tics from now
    double cost;
    int lethal;             // it could kill
} k_threathit_t;

// Called by combat on every level load.
void K_Threat_Reset(void);

// Once a tic, by combat while it lists the thinkers: Begin, then every
// missile and every live monster, then End.
void K_Threat_Begin(const struct mobj_s *player_mo);
void K_Threat_AddMissile(const struct mobj_s *mo);
void K_Threat_AddMonster(const struct mobj_s *mo, int visible);
void K_Threat_End(void);

// How many threats this tic could reach the player within K_THREAT_TICS,
// wherever it goes.
int K_Threat_Count(void);

// What going along the way would cost in health: path[t] is where the
// player is at the end of the tic t + 1 tics from now (tics up to
// K_THREAT_TICS), on the floor at `floor`. The hits that come later count
// less: the way can still change. worst, if not NULL, gets the costliest.
double K_Threat_PathCost(const k_vec2_t *path, int tics, double floor, k_threathit_t *worst);

// The projectile (or lost soul) that hit the player this tic, as far as can
// be told: one that was about to reach it last tic and is gone now; with
// how many tics it had been seen. NULL if none.
const char *K_Threat_HitBy(int *seen);

#ifdef __cplusplus
}
#endif

#endif
