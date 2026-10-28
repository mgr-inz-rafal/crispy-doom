//
// DESCRIPTION:
//	Karp jumps: straight runs across a gap to a floor the player could not
//	otherwise get to from the floors in between (karp/docs/plan.md, 3.2.2).
//	Gaps narrower than the player need no speed, since its box always
//	stands on a floor high enough; wider ones need a running jump. Both are
//	one thing here: the player's box is moved along a straight line, a tic
//	at a time, by the engine's rules (thrust and friction on the ground,
//	gravity in the air, the highest floor under the box, steps of 24,
//	headroom of 56, walls and solid things), and a jump is a run that ends
//	standing on such a floor.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_JUMP__
#define __K_JUMP__

#include "k_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int from_cell;          // the cell the run-up starts in
    int to_cell;            // the cell the player lands in
    k_vec2_t start;         // the run-up's start: the player stands here
    k_vec2_t edge;          // the last point on the takeoff floor
    k_vec2_t land;          // where the player touches down
    k_vec2_t dir;           // unit vector, the way of the run
    double speed;           // at the edge, from a standing start (units a tic)
    double flight;          // from the edge to the touchdown
    double from_floor;      // the floor it runs on, when it was found
    double land_floor;      // the floor it lands on, then
    int land_sector;        // the sector landed in
    int as_if_sector;       // found as if this sector were where a line
                            // sends it, -1 if as things were
    double as_if_floor;     // its floor and ceiling then
    double as_if_ceiling;
    int door;               // a door it was found with open that was closed,
                            // -1 if none
} k_jump_t;

// Called by the graph build, once the line grid and the portals are there,
// and the specials: looks for jumps off every edge the floor drops more than
// 24 from, across whatever lies beyond. A jump is kept only if it works with
// a margin: with 85% of the thrust, and from starts 6 units to either side.
// Then again around each sector a line moves, as if it were where the line
// sends it, with every door open: those jumps work only once it gets there
// (Doom II MAP09: one onto a floor a switch raises, one out of a door a
// switch opens).
void K_Jump_Find(void);

// The same, again, off the edges with an end in the box (min, max), once a
// sector there has moved: a door that opened, a floor that rose can make
// jumps that the heights at level load did not. Jumps already found stay as
// they are; new ones go after them. Returns how many are new.
int K_Jump_FindNear(k_vec2_t min, k_vec2_t max);

int K_Jump_Count(void);
const k_jump_t *K_Jump_Get(int jump);

// Whether the jump works now, with the margin of thrust: heights move
// (lifts, floors), and a solid thing can stand in the way. Judged once a
// tic.
int K_Jump_Possible(int jump);

#ifdef __cplusplus
}
#endif

#endif
