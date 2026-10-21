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

typedef struct
{
    int sector;
    int first_point;        // into k_map_t.points
    int num_points;         // closed convex polygon, counter-clockwise
    k_vec2_t centroid;      // area-weighted
    double area;
    k_vec2_t min, max;      // bounding box of the polygon
    // Leaves without floor have no polygon (num_points is 0). Harmless:
    int no_area;            // the BSP gives it no area, or a hairline sliver
    int behind_wall;        // its BSP region is solid space behind its own
                            // one-sided walls, e.g. inside a pillar
    // A real anomaly:
    int degenerate;         // its floor lies behind one of its two-sided
                            // segs (buggy nodes); the polygon is its BSP
                            // region minus what lies behind its walls
    int centroid_mismatch;  // the engine puts the centroid in another
                            // subsector
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
    int num_points;
    k_vec2_t *points;       // all subsector polygons, back to back

    k_vec2_t min, max;      // bounding box of all lines

    // Load-time sanity checks.
    int num_nodes;
    int no_area_subsectors;
    int behind_wall_subsectors;
    int degenerate_subsectors;
    int unreached_subsectors;   // not reachable through the BSP tree
    int centroid_mismatches;
    double build_ms;
} k_map_t;

typedef struct
{
    double x, y;
    double angle;           // radians, counter-clockwise from east
} k_pose_t;

// Called by Karp at the end of every P_SetupLevel.
void K_Map_Build(void);

const k_map_t *K_Map_Get(void);

k_lineclass_t K_Map_LineClass(int line);

// The subsector containing the point, or -1 when the point is outside
// every subsector polygon (in the void).
int K_Map_SubsectorAt(double x, double y);

// Live floor and ceiling heights of a sector.
void K_Map_SectorHeights(int sector, double *floor, double *ceiling);

// The local player's position and facing. Returns 0 when not in a level.
int K_Map_GetPlayer(k_pose_t *pose);

#ifdef __cplusplus
}
#endif

#endif
