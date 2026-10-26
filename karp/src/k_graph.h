//
// DESCRIPTION:
//	Karp navigation graph: which floor cells border each other, along
//	which stretch of boundary, and whether the player can cross it
//	(karp/docs/plan.md, 3.2). Rebuilt on every level load, after the
//	cells. Crossings are judged live, from the current sector heights, so
//	doors and lifts change the graph as they move.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_GRAPH__
#define __K_GRAPH__

#include "k_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int cell[2];            // the cells on either side
    k_vec2_t a, b;          // the shared stretch of their boundary
    int line;               // the linedef along it, -1 if none (the two
                            // cells are in the same sector)
    int teleport_from;      // along a teleport line: the cell on its front
                            // side, crossing out of which teleports the
                            // player; -1 otherwise
} k_portal_t;

// A teleporter: the walk-over lines with one tag that teleport the player
// when crossed from their front side, and where to (EV_Teleport: the first
// teleport destination in the first sector with the tag).
typedef struct
{
    int tag;
    k_vec2_t to;            // the destination
    double angle;           // the way the player faces on arrival, radians
    int cell;               // the cell the destination is in, -1 if there
                            // is none (crossing the lines does nothing)
    k_vec2_t min, max;      // bounding box of its lines
} k_teleport_t;

typedef struct
{
    int portal;
    int neighbor;           // the cell on the far side
} k_link_t;

// Whether the player can cross a portal, and if not, why.
typedef enum
{
    K_CROSS_OPEN,
    K_CROSS_WALL,           // the line is one-sided
    K_CROSS_BLOCKING,       // the line blocks players (ML_BLOCKING)
    K_CROSS_STEP,           // the floor beyond is more than 24 units higher
    K_CROSS_HEADROOM,       // less than 56 units between floor and ceiling,
                            // e.g. a closed door that cannot be opened
                            // from this side, or not high enough
    K_CROSS_LOCKED,         // a door that needs a key the player lacks
    K_CROSS_NARROW,         // a gap narrower than the player, e.g. between
                            // bars or raised blocks

    // Passable after an action:
    K_CROSS_DOOR,           // a closed door the player can open: use its
                            // line from this side
    K_CROSS_LIFT,           // a raised lift the player can lower: use its
                            // line from this side, then step on
    K_CROSS_RIDE,           // off a lift that is down: wait on it until it
                            // rises

    // Passable, but not into the cell beyond:
    K_CROSS_TELEPORT,       // a teleport line, from its front side: the
                            // player lands at the teleporter's destination
    K_NUM_CROSSINGS
} k_crossing_t;

// What the mover of a sector (a door, lift, floor...) is doing.
typedef enum
{
    K_MOVER_NONE,           // nothing is moving it
    K_MOVER_UP,
    K_MOVER_DOWN,
    K_MOVER_WAITING,        // a door open at the top, a lift at the bottom
    K_MOVER_OTHER,          // some other mover; direction unknown
} k_mover_t;

typedef struct
{
    unsigned int generation;    // the map generation this graph belongs to

    int num_portals;
    k_portal_t *portals;
    int num_links;
    k_link_t *links;            // grouped by cell
    int *first_link;            // per cell, into links
    int *link_count;            // per cell

    int num_teleports;
    k_teleport_t *teleports;

    // Load-time sanity checks.
    int holes;                  // stretches of cell border, away from walls,
                                // that border no neighbour
    double hole_length;         // their total length
    int isolated_cells;         // cells without neighbours
    int components;             // groups of cells joined by portals
    int largest_component;      // cells in the biggest group
    double build_ms;
} k_graph_t;

// Called by Karp after K_Map_Build.
void K_Graph_Build(void);

const k_graph_t *K_Graph_Get(void);

// Whether the player can cross the portal when leaving the given cell.
k_crossing_t K_Graph_CanCross(int portal, int from_cell);

// Where to cross the portal: as clear of walls and other obstacles as the
// portal allows (enough not to clip a wall corner), otherwise as near its
// middle as possible.
k_vec2_t K_Graph_PortalPoint(int portal, int from_cell);

// Open, passable after an action (a door, a lift), or a teleporter.
int K_Graph_Passable(k_crossing_t crossing);

// How long the action a crossing needs is expected to take, in tics: 0
// for an open one. For a teleporter, the time the player stands frozen
// on arrival.
double K_Graph_CrossingWait(int portal, int from_cell);

// The teleporter (index into teleports) that crossing the portal out of
// the given cell sends the player to, or -1. Live: a one-shot teleport
// line loses its special once used. Whatever the heights say: a crossing
// too high to step onto still leads there once it is fixed.
int K_Graph_Teleport(int portal, int from_cell);

const char *K_Graph_CrossingName(k_crossing_t crossing);

// The key a locked door needs (a k_key_t), or -1.
int K_Graph_LockKey(int portal);

// K_CROSS_NARROW: the sectors that make the gap narrow (the far side of
// two-sided lines the player cannot get past, e.g. raised bars), up to max.
// Returns how many.
int K_Graph_NarrowSectors(int portal, int from_cell, int *sectors, int max);

// Its name ("blue", "yellow", "red"), or NULL.
const char *K_Graph_LockName(int portal);

// How far a point on the floor is from the nearest line a player standing
// there could not overlap (a wall, an impassable line, a side too high to
// step onto or too low to fit under), up to reach; reach if none is
// nearer, 0 off the floor.
double K_Graph_Clearance(k_vec2_t at, double reach);

k_mover_t K_Graph_SectorMover(int sector);

#ifdef __cplusplus
}
#endif

#endif
