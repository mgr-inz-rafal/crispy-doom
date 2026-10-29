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
    int jump;               // a jump (K_Jump_Get), not a shared stretch of
                            // border: crossed from cell[0] only, a is the
                            // run-up's start and b the touchdown; -1
                            // otherwise
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
    K_CROSS_THING,          // a solid thing stands there (k_block_t): a
                            // decoration, a barrel
    K_CROSS_GAP,            // a jump that does not work now (the heights
                            // moved, something stands in the way)

    // Passable after an action:
    K_CROSS_DOOR,           // a closed door the player can open: use its
                            // line from this side
    K_CROSS_LIFT,           // a raised lift the player can lower: use its
                            // line from this side, then step on
    K_CROSS_RIDE,           // off a lift that is too low (or, coming down,
                            // too high): wait on it until it gets there

    // Passable, but not into the cell beyond:
    K_CROSS_TELEPORT,       // a teleport line, from its front side: the
                            // player lands at the teleporter's destination

    // Passable, running straight at full speed:
    K_CROSS_JUMP,           // a jump (k_jump.h): from the run-up's start
                            // over a gap to the touchdown
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

// Once a tic in a level: when a sector stops moving (a door open, a floor
// risen), looks for jumps again around it and adds the new ones (k_jump.h).
void K_Graph_Tick(void);

// Whether a sector is a manual door (opened by using its own line).
int K_Graph_IsDoor(int sector);

// Whether a sector is a lift (some line with its tag lowers it, and it comes
// back up).
int K_Graph_IsLift(int sector);

// Where a door in the sector opens to: 4 under the lowest neighbouring
// ceiling.
double K_Graph_DoorTop(int sector);

// A sector's ceiling, taking a door already opening (a switch opened it) as
// open, to where it is going.
double K_Graph_OpeningCeiling(int sector);

// Whether the player can cross the portal when leaving the given cell.
k_crossing_t K_Graph_CanCross(int portal, int from_cell);

// Where to cross the portal: as clear of walls and other obstacles as the
// portal allows (enough not to clip a wall corner), otherwise as near its
// middle as possible.
k_vec2_t K_Graph_PortalPoint(int portal, int from_cell);

// Open, passable after an action (a door, a lift), a teleporter or a jump.
int K_Graph_Passable(k_crossing_t crossing);

// Whether going through the cell from one stretch of its border to another
// (a portal, or a point: a == b) means squeezing through a gap narrower
// than the player inside it, where no portal crosses: a closed block beside
// a wall, mid-cell (Doom II MAP21's yellow bars). Judged live, like a
// crossing; where gets the gap's middle.
int K_Graph_Pinched(int cell, k_vec2_t from_a, k_vec2_t from_b,
                    k_vec2_t to_a, k_vec2_t to_b, k_vec2_t *where);

// A gap inside a cell, by the cell and where K_Graph_Pinched put it: the
// sectors that make it narrow, as K_Graph_NarrowSectors (Doom II MAP11: a
// fence of pillars 16 apart, which lines open or lower), up to max; and
// whether it is still narrower than the player.
int K_Graph_PinchSectors(int cell, k_vec2_t at, int *sectors, int max);
int K_Graph_PinchNarrow(int cell, k_vec2_t at);

// How long the action a crossing needs is expected to take, in tics: 0
// for an open one. For a teleporter, the time the player stands frozen
// on arrival. beyond, if not NULL, gets the part of it the player spends
// past the crossing: frozen at a teleporter's destination, or aboard a
// lift lowered by its switch (its wait at the bottom, the ride up); the
// rest is spent in the cell it crosses from.
double K_Graph_CrossingWait(int portal, int from_cell, double *beyond);

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

// The linedefs (indices into the map's lines) whose grid cells reach into
// the box from min to max, each once, up to max_found: those it may touch.
// Returns how many.
int K_Graph_LinesNear(k_vec2_t min, k_vec2_t max, int *found, int max_found);

// How far a point on the floor is from the nearest line a player standing
// there could not overlap (a wall, an impassable line, a side too high to
// step onto or too low to fit under) or standing block, up to reach; reach
// if none is nearer, 0 off the floor. Distances are measured as the engine
// collides the player's square box: the larger of the x and y distances,
// so a box of radius r overlaps what is nearer than r.
double K_Graph_Clearance(k_vec2_t at, double reach);

// A line near a point that stops the player.
typedef struct
{
    k_vec2_t away;          // unit vector from the line's nearest point to
                            // the point
    double distance;        // box distance (K_Graph_Clearance)
} k_nearline_t;

// The lines within reach of a point that a player standing there, on the
// given floor, could not overlap, up to max (not blocks: the engine slides
// the player along lines, not things; K_Map_BlocksNear gives those).
// Returns how many. Judged from
// the floor the player stands on, not the one under its centre: over a
// ledge's edge, the ledge would look like a wall to climb, and the player
// would be steered off it (Doom II MAP17's bridge over a pit).
int K_Graph_ObstaclesNear(k_vec2_t at, double floor, double reach, k_nearline_t *found,
                          int max);

// How far a point is from the nearest edge within reach that a player
// standing there on the given floor could drop over but not climb back: a
// two-sided line it can get onto, with the other side's floor more than
// 24 below. Box distance, as K_Graph_Clearance; reach if none is nearer.
double K_Graph_DropDistance(k_vec2_t at, double floor, double reach);

k_mover_t K_Graph_SectorMover(int sector);

// Whether a sector's floor is lowering (a floor, not a lift, on its way
// down): no time to stand about on it (Doom II MAP08's sinking pillars).
int K_Graph_FloorSinking(int sector);

// A lift the player rode turned back before it could get off: something
// the map does not show is in its way (a monster at its edge, a barrel).
// Each time makes riding that lift count as a longer wait, until a ride on
// it works (K_Graph_LiftRidden). Returns how many times so far.
int K_Graph_LiftTurnedBack(int sector);
void K_Graph_LiftRidden(int sector);

// A sector whose floor, ceiling, door or lift is on its way somewhere, or
// -1 if none. Perpetual lifts and crushers, which never stop, do not count.
int K_Graph_MovingSector(void);

#ifdef __cplusplus
}
#endif

#endif
