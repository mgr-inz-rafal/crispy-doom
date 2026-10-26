//
// DESCRIPTION:
//	Karp map reader: Karp's own copy of the loaded level's geometry,
//	rebuilt on every level load (karp/docs/plan.md, 3.2).
//
//	Plain C types only: the console (C++) includes this header.
//	Coordinates are map units, y pointing north as in the automap.
//

#ifndef __K_MAP__
#define __K_MAP__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    double x, y;
} k_vec2_t;

typedef struct
{
    k_vec2_t v1, v2;
    int flags;              // ML_* linedef flags
    int special;
    int front_sector;       // index into the level's sectors, -1 if none
    int back_sector;        // -1 for one-sided lines
} k_line_t;

// How the automap would colour a line, from live sector heights.
typedef enum
{
    K_LINE_ONE_SIDED,       // solid wall
    K_LINE_FLOOR_STEP,      // two-sided, floor heights differ
    K_LINE_CEILING_STEP,    // two-sided, only ceiling heights differ
    K_LINE_TWO_SIDED,       // two-sided, same heights on both sides
} k_lineclass_t;

// A convex piece of a subsector's floor: the nodes of the navigation
// graph. A subsector's segs are guaranteed to be convex, but its floor is
// not: a wall corner inside its BSP region leaves floor on both sides of
// the wall's line. Most subsectors have exactly one cell.
typedef struct
{
    int subsector;
    int first_point;        // into k_map_t.points
    int num_points;         // convex, counter-clockwise
    k_vec2_t centroid;      // area-weighted
    double area;
    k_vec2_t min, max;      // bounding box
    int centroid_mismatch;  // the engine puts the centroid in another
                            // subsector
} k_cell_t;

typedef struct
{
    int sector;
    int first_cell;         // into k_map_t.cells
    int num_cells;          // 0 for leaves without floor
    k_vec2_t centroid;      // of all its floor; placed among its segs if
                            // it has none
    double area;            // total floor area
    k_vec2_t min, max;      // bounding box of its cells

    // Leaves without floor have no cells. Harmless:
    int no_area;            // the BSP gives it no area, or a hairline sliver
    int behind_wall;        // its whole BSP region is solid space behind
                            // walls, e.g. inside a pillar

    // Some of its floor lies, judged by the surrounding lines, in another
    // sector: sector tricks or buggy nodes. Informational: the engine (and
    // so the navigation graph) still uses the subsector's own sector there.
    int foreign_floor;
} k_subsector_t;

typedef struct
{
    int loaded;             // false until the first level loads
    unsigned int generation;    // bumped on every level load
    char name[9];           // map lump name, e.g. "E1M1"

    int num_lines;
    k_line_t *lines;
    int num_sectors;

    int num_subsectors;
    k_subsector_t *subsectors;
    int num_cells;
    k_cell_t *cells;
    int num_points;
    k_vec2_t *points;       // all cell polygons, back to back

    k_vec2_t min, max;      // bounding box of all lines

    // Load-time sanity checks.
    int num_nodes;
    int split_subsectors;       // subsectors whose floor needs several cells
    int no_area_subsectors;
    int behind_wall_subsectors;
    int foreign_floor_subsectors;
    int unreached_subsectors;   // not reachable through the BSP tree
    int centroid_mismatches;    // cells whose centroid the engine places in
                                // another subsector
    double build_ms;
} k_map_t;

typedef struct
{
    double x, y;
    double angle;           // radians, counter-clockwise from east
    double momx, momy;      // momentum, map units per tic
    int onground;           // movement input only works on the ground
    int frozen;             // tics left that it can neither move nor turn
                            // (just teleported)
} k_pose_t;

// Called by Karp at the end of every P_SetupLevel.
void K_Map_Build(void);

const k_map_t *K_Map_Get(void);

k_lineclass_t K_Map_LineClass(int line);

// The cell containing the point (within 1 unit), or -1 when the point is
// on no floor (in the void).
int K_Map_CellAt(double x, double y);

// The cell nearest to the point, if one is within max_distance (0 when the
// point is inside it); -1 otherwise. Scans every cell.
int K_Map_NearestCell(double x, double y, double max_distance);

// The subsector containing the point, or -1 when it is on no floor.
int K_Map_SubsectorAt(double x, double y);

// Live floor and ceiling heights of a sector.
void K_Map_SectorHeights(int sector, double *floor, double *ceiling);

// The local player's position and facing. Returns 0 when not in a level.
int K_Map_GetPlayer(k_pose_t *pose);

#ifdef __cplusplus
}
#endif

#endif
