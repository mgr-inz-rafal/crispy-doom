//
// DESCRIPTION:
//	Karp navigation: plans the route to the current goal and (from M6)
//	follows it (karp/docs/plan.md, 3.4).
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_NAV__
#define __K_NAV__

#include "k_path.h"

#ifdef __cplusplus
extern "C" {
#endif

// Drops the goal and its route; called on every level load.
void K_Nav_Reset(void);

// Plans a route from the player to (x, y) and logs the outcome. Returns
// non-zero if one was found; K_Nav_GetPath has the route, or why there is
// none, either way.
int K_Nav_SetGoal(double x, double y);

// The route to the current goal, or NULL when there is no goal.
const k_path_t *K_Nav_GetPath(void);

#ifdef __cplusplus
}
#endif

#endif
