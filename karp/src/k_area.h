//
// DESCRIPTION:
//	Karp areas (karp/docs/plan.md, 7.8: C2): the level cut into room-like
//	areas - rooms split at doors, lifts, teleporters and narrow openings,
//	large open spaces cut into pieces of bounded size - and, live, how
//	dangerous each is: the monsters inside it, and those that could hit
//	the player in it, by sight and distance; whether the player has been
//	there; how much of its floor hurts.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_AREA__
#define __K_AREA__

#include "k_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    k_vec2_t min, max;      // bounding box of its cells
    k_vec2_t center;        // of its floor, area-weighted
    k_vec2_t spot;          // a point on its floor: its largest cell's middle
    double floor_area;
    int first_cell;         // into k_areamap_t.cells
    int num_cells;
    int passage;            // a door or a lift: a way between areas, no room

    // Live:
    int monsters;           // live monsters inside it
    int awake;              // of those, awake
    double inside;          // what fighting them would cost in health, the
                            // asleep at half
    double danger;          // what the monsters that could hit the player
                            // in it would cost: inside it or not, by sight
                            // and distance, the asleep at half; looked at
                            // about once a second
    double awake_danger;    // of that, the awake monsters'
    int visited;            // the player has been in it
    double hurts;           // the share of its floor that hurts
} k_area_t;

typedef struct
{
    unsigned int generation;    // the map generation it belongs to
    int num_areas;
    k_area_t *areas;
    int *cells;             // each area's cells, back to back
    int *area_of;           // per map cell, -1 for none
    int passages;
    double build_ms;
} k_areamap_t;

// Called by Karp after K_Graph_Build.
void K_Area_Build(void);

// Once a tic in a level, after combat has listed the monsters.
void K_Area_Tick(void);

const k_areamap_t *K_Area_Get(void);

// The area a cell belongs to, -1 if none.
int K_Area_Of(int cell);

// Whether an area is safe: no live monster inside, and its danger below
// what a lone zombieman would cost.
int K_Area_Safe(int area);

#ifdef __cplusplus
}
#endif

#endif
