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
    int loaded;             // false until the first level loads
    unsigned int generation;    // bumped on every level load
    char name[9];           // map lump name, e.g. "E1M1"

    int num_lines;
    k_line_t *lines;
    int num_sectors;

    k_vec2_t min, max;      // bounding box of all lines
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

// The local player's position and facing. Returns 0 when not in a level.
int K_Map_GetPlayer(k_pose_t *pose);

#ifdef __cplusplus
}
#endif

#endif
