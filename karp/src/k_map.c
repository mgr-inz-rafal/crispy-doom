//
// DESCRIPTION:
//	Karp map reader. Built from the engine's level data at the end of
//	P_SetupLevel. All level memory is PU_LEVEL and is freed on the next
//	load, so Karp keeps its own copies and rebuilds them every time.
//
//	Sector heights are always read live from the engine: floors move, and
//	loading a savegame changes them after P_SetupLevel.
//

#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomstat.h"
#include "r_state.h"
#include "w_wad.h"
#include "p_setup.h"

#include "k_log.h"
#include "k_map.h"

#define K_PI 3.14159265358979323846

static k_map_t map;

static double FixedToMap(fixed_t value)
{
    return (double) value / FRACUNIT;
}

static int SectorIndex(const sector_t *sector)
{
    if (sector == NULL || sector < sectors || sector >= sectors + numsectors)
    {
        return -1;
    }

    return (int) (sector - sectors);
}

static void ExtendBounds(const k_vec2_t *point)
{
    map.min.x = SDL_min(map.min.x, point->x);
    map.min.y = SDL_min(map.min.y, point->y);
    map.max.x = SDL_max(map.max.x, point->x);
    map.max.y = SDL_max(map.max.y, point->y);
}

void K_Map_Build(void)
{
    Uint64 start = SDL_GetPerformanceCounter();
    int i;

    free(map.lines);
    map.lines = malloc(SDL_max(numlines, 1) * sizeof(*map.lines));
    map.num_lines = numlines;
    map.num_sectors = numsectors;

    map.name[0] = '\0';

    if (maplumpinfo != NULL)
    {
        // Lump names are not null-terminated when all 8 characters are used.
        memcpy(map.name, maplumpinfo->name, 8);
        map.name[8] = '\0';
    }

    map.min.x = map.min.y = 0.0;
    map.max.x = map.max.y = 0.0;

    for (i = 0; i < numlines; ++i)
    {
        const line_t *source = &lines[i];
        k_line_t *line = &map.lines[i];

        line->v1.x = FixedToMap(source->v1->x);
        line->v1.y = FixedToMap(source->v1->y);
        line->v2.x = FixedToMap(source->v2->x);
        line->v2.y = FixedToMap(source->v2->y);
        line->flags = source->flags;
        line->special = source->special;
        line->front_sector = SectorIndex(source->frontsector);
        line->back_sector = SectorIndex(source->backsector);

        if (i == 0)
        {
            map.min = map.max = line->v1;
        }

        ExtendBounds(&line->v1);
        ExtendBounds(&line->v2);
    }

    map.loaded = 1;
    ++map.generation;

    K_Log(K_LOG_MAP, K_LOG_INFO, "%s: %d lines, %d sectors, %.1f ms", map.name,
          map.num_lines, map.num_sectors,
          (SDL_GetPerformanceCounter() - start) * 1000.0
              / SDL_GetPerformanceFrequency());
}

const k_map_t *K_Map_Get(void)
{
    return &map;
}

k_lineclass_t K_Map_LineClass(int index)
{
    const k_line_t *line = &map.lines[index];
    const sector_t *front, *back;

    if (line->front_sector < 0 || line->back_sector < 0)
    {
        return K_LINE_ONE_SIDED;
    }

    front = &sectors[line->front_sector];
    back = &sectors[line->back_sector];

    if (front->floorheight != back->floorheight)
    {
        return K_LINE_FLOOR_STEP;
    }

    if (front->ceilingheight != back->ceilingheight)
    {
        return K_LINE_CEILING_STEP;
    }

    return K_LINE_TWO_SIDED;
}

int K_Map_GetPlayer(k_pose_t *pose)
{
    const mobj_t *mo = players[consoleplayer].mo;

    if (gamestate != GS_LEVEL || mo == NULL)
    {
        return 0;
    }

    pose->x = FixedToMap(mo->x);
    pose->y = FixedToMap(mo->y);
    pose->angle = mo->angle * (2.0 * K_PI / 4294967296.0);
    return 1;
}
