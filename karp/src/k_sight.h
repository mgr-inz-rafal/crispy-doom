//
// DESCRIPTION:
//	Karp sight checks (karp/docs/plan.md, 7.2): whether a straight line
//	between two points is unobstructed, decided as P_CheckSight decides
//	it, without its side effects.
//
//	Plain C types only, except K_Sight_Things, which takes the engine's
//	mobjs (only C files use it).
//

#ifndef __K_SIGHT__
#define __K_SIGHT__

#ifdef __cplusplus
extern "C" {
#endif

struct mobj_s;

// Called by Karp on every level load.
void K_Sight_Reset(void);

// Whether an eye at (x1, y1, eye) sees any part of the column at (x2, y2)
// from bottom to top: the REJECT table first, then the trace through the
// BSP that P_CheckSight makes, in the engine's own fixed-point maths.
int K_Sight_Check(double x1, double y1, double eye, double x2, double y2, double bottom,
                  double top);

// P_CheckSight(looker, target): from the looker's eyes (a quarter of its
// height down from its top) to any part of the target.
int K_Sight_Things(const struct mobj_s *looker, const struct mobj_s *target);

// Whether a player's shot from (x1, y1, z) - the gun is 8 above the
// middle of the player - toward (x2, y2) would find the column from bottom
// to top there: as K_Sight_Check, but within the autoaim's reach up and
// down (P_AimLineAttack: a slope of 100/160 either way). Seeing a row of a
// monster's pixels over a ledge is not hitting it.
int K_Sight_Aim(double x1, double y1, double z, double x2, double y2, double bottom,
                double top);

#ifdef __cplusplus
}
#endif

#endif
