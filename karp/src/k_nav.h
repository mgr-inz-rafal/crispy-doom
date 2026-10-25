//
// DESCRIPTION:
//	Karp navigation: plans the route to the current goal and follows it
//	(karp/docs/plan.md, 3.4).
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_NAV__
#define __K_NAV__

#include "k_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    K_NAV_IDLE,             // no goal
    K_NAV_DRIVING,          // following the route to the goal
    K_NAV_ARRIVED,          // reached the goal; reported once, then IDLE
    K_NAV_FAILED,           // no route, or stuck; the route stays to show why
} k_navstate_t;

// Controller settings, adjustable in the console's Bot tab.
typedef struct
{
    double max_speed;       // map units per tic; 16.7 is running speed
    double braking;         // deceleration assumed when slowing down for
                            // corners and the goal, units per tic per tic
    double advance_radius;  // a waypoint on the way counts as reached this
                            // close
    double max_turn;        // degrees per tic
} k_navtuning_t;

typedef struct
{
    k_navstate_t state;
    int target;             // index into the route's points: the waypoint
                            // being driven to
    int route_cell;         // index into the route's cells: the last one
                            // the player was in
    double remaining;       // distance left along the route
    double speed;           // units per tic
    int tics;               // since the goal was set
    int replans;            // since the goal was set
    int stuck;              // times stuck in a row
    double prediction_error;    // average distance between where the
                                // controller expected the player after a
                                // tic and where it was, over ~1 second
    char problem[200];      // K_NAV_FAILED: why
} k_navstatus_t;

// The bot's input for one tic, in ticcmd units.
typedef struct
{
    int forward;
    int side;
    int turn;               // angleturn: 65536 is a full circle
} k_command_t;

// Drops the goal and its route; called on every level load.
void K_Nav_Reset(void);

// Plans a route from the player to (x, y) and logs the outcome. Returns
// non-zero if one was found; K_Nav_GetPath has the route, or why there is
// none, either way.
int K_Nav_SetGoal(double x, double y);

// The route to the current goal, or NULL when there is no goal.
const k_path_t *K_Nav_GetPath(void);

const k_navstatus_t *K_Nav_GetStatus(void);

k_navtuning_t *K_Nav_Tuning(void);

// Once per game tic while the bot drives: follows the player along the
// route, replans when it leaves the route or gets stuck, and notices
// arrival.
k_navstate_t K_Nav_Tick(void);

// The bot's input for the command that will run as game tic `tic`.
void K_Nav_BuildCommand(int tic, k_command_t *command);

#ifdef __cplusplus
}
#endif

#endif
