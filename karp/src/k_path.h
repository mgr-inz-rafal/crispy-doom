//
// DESCRIPTION:
//	Karp pathfinder: A* across the navigation graph, from a start point to
//	a goal point (karp/docs/plan.md, 3.3).
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_PATH__
#define __K_PATH__

#include "k_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    K_PATH_FOUND,
    K_PATH_NO_START,        // the start is not on any floor
    K_PATH_NO_GOAL,         // the goal is not on any floor
    K_PATH_BLOCKED,         // every route crosses something the player
                            // cannot cross now, e.g. a step too high
    K_PATH_UNCONNECTED,     // no route at all, e.g. the goal is in a closed
                            // off area, or one reached only by a teleporter
                            // that is used up
} k_pathresult_t;

// How a leg of the route is taken.
typedef enum
{
    K_LEG_WALK,
    K_LEG_TELEPORT,         // not walked: the teleporter takes the player
    K_LEG_JUMP,             // run straight at full speed and fly
} k_leg_t;

typedef struct
{
    k_pathresult_t result;
    k_vec2_t start, goal;

    // The route from the start's cell to the goal's cell; portals[i] joins
    // cells[i] and cells[i + 1], or is a teleport line crossed out of
    // cells[i] that lands the player in cells[i + 1], or a jump from
    // cells[i] to cells[i + 1]. When blocked, this is the route with the
    // fewest blocked crossings, to show why.
    int num_cells;
    int *cells;
    int *portals;

    // The line to follow: the start, a point on each portal (its middle,
    // moved clear of walls if need be: K_Graph_PortalPoint), the goal.
    // Each leg runs between two points on the border of one convex cell,
    // so it stays inside that cell and cannot cut through a wall corner.
    // leg_cells[i] is the cell (index into cells) of the leg from
    // points[i] to points[i + 1].
    //
    // At a teleporter the route goes on from a point just past its line
    // (to be sure to cross it) to its destination: legs[i] is
    // K_LEG_TELEPORT for that leg, which is not walked. A jump's leg runs
    // from the run-up's start to the touchdown, K_LEG_JUMP. Either belongs
    // to the cell after.
    int num_points;
    k_vec2_t *points;
    int *leg_cells;
    unsigned char *legs;    // k_leg_t
    double length;          // walked or run, without the teleports

    // K_PATH_BLOCKED: the first crossing on the route that is blocked (the
    // portal, and the cell it is crossed from), why, and by how much (the
    // height of the step, or the headroom left); and how many there are.
    int blocked_portal;
    int blocked_from;
    k_crossing_t blocked_by;
    double blocked_height;
    int blocked_count;

    // Or a gap inside blocked_from, narrower than the player, on the way
    // through it (K_Graph_Pinched): blocked_by is K_CROSS_NARROW, the gap is
    // at blocked_at, and blocked_portal is the way out of the cell (-1 if
    // the route ends there and began there too). Nothing fixes it.
    int blocked_pinch;
    k_vec2_t blocked_at;

    int searched;           // search nodes expanded
    double search_ms;

    int capacity;           // internal: room in cells, portals and points
} k_path_t;

// Plans a route from start to goal over the crossings that are open now.
// start_cell is the cell the start is on, when that is known better than
// from the point (the player's standing cell: K_Map_GetPlayer), or -1 to
// look it up. The arrays in path are reused from call to call.
void K_Path_Find(k_path_t *path, k_vec2_t start, int start_cell, k_vec2_t goal);

// What a blocked crossing adds to the cost of a route. More than any route
// on a map could cost, so open routes always win; set by the brain, which
// knows which blocked crossings it could get past (a key, a switch).
// Without one, every blocked crossing costs the same.
typedef double (*k_blockcost_t)(int portal, int from_cell, k_crossing_t crossing);

void K_Path_SetBlockCost(k_blockcost_t cost);

// The same for a gap inside a cell (K_Graph_Pinched), by the cell and where
// the gap is. Without one, it costs as any blocked crossing.
typedef double (*k_pinchcost_t)(int cell, k_vec2_t at);

void K_Path_SetPinchCost(k_pinchcost_t cost);

void K_Path_Free(k_path_t *path);

// "found", "blocked", ...
const char *K_Path_ResultName(k_pathresult_t result);

// One line about the outcome, e.g. "23 cells, 1840 units" or "a step 64
// units high at line 312 (1024,-320)".
void K_Path_Describe(const k_path_t *path, char *buf, int buf_len);

#ifdef __cplusplus
}
#endif

#endif
