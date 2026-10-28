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

// What the bot is doing at a door or lift on the route, or at a switch it
// was sent to.
typedef enum
{
    K_ACTION_NONE,
    K_ACTION_DOOR,          // opening a door
    K_ACTION_LIFT,          // calling a lift down
    K_ACTION_RIDE,          // on a lift, waiting for it to rise
    K_ACTION_USE,           // pressing the switch at the goal
    K_ACTION_SHOOT,         // shooting the gun switch at the goal
} k_navaction_t;

// What to do at the goal.
typedef enum
{
    K_GOAL_POINT,           // just get there
    K_GOAL_USE_LINE,        // press use on a line (a switch)
    K_GOAL_CROSS_LINE,      // walk across a line (a walk-over trigger)
    K_GOAL_SHOOT_LINE,      // shoot a line (a gun switch), with a hitscan
                            // weapon
} k_goalkind_t;

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
    k_goalkind_t goal_kind;
    int goal_line;          // the line to work at the goal, -1 if none
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
    k_navaction_t action;
    int action_line;        // the line it uses, -1 if none
    char action_text[96];   // e.g. "door at line 151: waiting"
    char problem[200];      // K_NAV_FAILED: why
    int blocked;            // K_NAV_FAILED: the way shut on the way, e.g. a
                            // door closed; a route is there, but blocked
} k_navstatus_t;

// The bot's input for one tic, in ticcmd units.
typedef struct
{
    int forward;
    int side;
    int turn;               // angleturn: 65536 is a full circle
    int use;                // press use (BT_USE)
    int attack;             // fire (BT_ATTACK)
    int change;             // switch to `weapon` (BT_CHANGE)
    int weapon;             // a weapon slot, as the number keys pick it
} k_command_t;

// Drops the goal and its route; called on every level load.
void K_Nav_Reset(void);

// Plans a route from the player to (x, y) and logs the outcome. Returns
// non-zero if one was found; K_Nav_GetPath has the route, or why there is
// none, either way.
int K_Nav_SetGoal(double x, double y);

// Where to stand to work a line: in front of a switch or gun switch (its
// front side, where use works), or just past a line to walk across, on the
// side away from the player. Returns 0 if there is no floor there.
int K_Nav_LineGoalPoint(int line, k_goalkind_t kind, double *x, double *y);

// Like K_Nav_SetGoal, to the point K_Nav_LineGoalPoint gives; on arrival
// a switch is pressed. Walking across needs nothing more: the route takes
// the player over the line.
int K_Nav_SetLineGoal(int line, k_goalkind_t kind);

// Drops the goal: the player stands still (e.g. while a door it opened
// with a switch rises).
void K_Nav_Clear(void);

// The route to the current goal, or NULL when there is no goal.
const k_path_t *K_Nav_GetPath(void);

// Whether the route's leg from point `leg` to the next is taken carefully,
// near an edge the player could fall over: into a careful waypoint (a
// slower corner, braked for early) or out of it (drift cancelled before
// speeding up). Marked when the route is planned.
int K_Nav_CarefulLeg(int leg);

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
