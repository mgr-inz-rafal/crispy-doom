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
    K_PATH_UNCONNECTED,     // no route at all, e.g. the goal is in an area
                            // reached only by teleporter
} k_pathresult_t;

typedef struct
{
    k_pathresult_t result;
    k_vec2_t start, goal;

    // The route from the start's cell to the goal's cell; portals[i] joins
    // cells[i] and cells[i + 1]. When blocked, this is the route with the
    // fewest blocked crossings, to show why.
    int num_cells;
    int *cells;
    int *portals;

    // The line to follow: the start, the middle of each portal, the goal.
    // Each leg runs between two points on the border of one convex cell,
    // so it stays inside that cell and cannot cut through a wall corner.
    // leg_cells[i] is the cell (index into cells) of the leg from
    // points[i] to points[i + 1].
    int num_points;
    k_vec2_t *points;
    int *leg_cells;
    double length;

    // K_PATH_BLOCKED: the first crossing on the route that is blocked, why,
    // and by how much (the height of the step, or the headroom left).
    int blocked_portal;
    k_crossing_t blocked_by;
    double blocked_height;

    int searched;           // search nodes expanded
    double search_ms;

    int capacity;           // internal: room in cells, portals and points
} k_path_t;

// Plans a route from start to goal over the crossings that are open now.
// The arrays in path are reused from call to call.
void K_Path_Find(k_path_t *path, k_vec2_t start, k_vec2_t goal);

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
