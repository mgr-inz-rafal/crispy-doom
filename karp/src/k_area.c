//
// DESCRIPTION:
//	Karp areas (karp/docs/plan.md, 7.8: C2).
//
//	Built on every level load, from the cells and the navigation graph:
//	cells join while the floor goes on across the crossing between them
//	(open both ways; a solid thing over it, or within a sector a sliver too
//	narrow to cross, does not part the floor), not into a door or lift, and
//	not a narrow opening - two sectors that meet over
//	less than NARROW of open border all told (a doorway, a corridor's
//	mouth) - so rooms come apart at their doors and doorways; teleporters,
//	jumps, drops and lines that block are never joined across. Each area
//	grows from the cell furthest south-west left, nearest first, while its
//	bounding box stays within SPAN either way: large open spaces come in
//	pieces (the user: a demon far off in the same open field is no danger
//	here). Areas smaller than SMALL join the neighbour they share most
//	border with, up to MERGE_SPAN. A door's or lift's cells are an area of
//	their own, a passage.
//
//	Live: the monsters inside each area, every tic; its danger, a few areas
//	every tic, each about once a second (REFRESH_TICS) - every live monster
//	within DANGER_REACH that can see one of its sample points (up to
//	SAMPLES cells' middles, spread over it) or stands in it, by what a fight
//	with its kind costs, in full within DANGER_FULL and less and less
//	beyond, the asleep at ASLEEP_SHARE (the user). A sector shut at load (a
//	door a switch opens, bars) is a passage too. Missiles never count:
//	they pass. The areas the player has stood in are visited.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomdef.h"
#include "doomstat.h"

#include "k_area.h"
#include "k_combat.h"
#include "k_graph.h"
#include "k_log.h"
#include "k_sight.h"

#define NARROW 128.0
#define SPAN 768.0
#define SMALL (128.0 * 128.0)
#define MERGE_SPAN 960.0

#define SAMPLES 4
#define REFRESH_TICS TICRATE
#define DANGER_REACH 1024.0
#define DANGER_FULL 512.0
#define ASLEEP_SHARE 0.5
#define PLAYER_HEIGHT 56.0

// Safe below what a lone zombieman would cost (k_combat.c: 5).
#define SAFE_DANGER 5.0

#define MAX_STEP 24.0

typedef struct
{
    int a, b;               // sectors, a < b
    double length;          // open border between them
} pair_t;

static k_areamap_t areamap;
static int area_capacity;
static int cell_capacity;

// Per portal, what it is to the areas (JoiningPortals), for the map
// generation the areas belong to.
static unsigned char *portal_joins;
static int num_portal_joins;

// Per area: its sample cells; and the next area to look at.
static int (*samples)[SAMPLES];
static int *num_samples;
static int next_refresh;

// Per monster (K_Combat_Monster), this tic: the area it stands in, -1 if
// none.
static int *monster_area;
static int monster_capacity;

static int CellSector(const k_map_t *map, int cell)
{
    return map->subsectors[map->cells[cell].subsector].sector;
}

// A door or a lift, or shut at load (a door a switch opens, bars).
static boolean Passage(int sector)
{
    double floor, ceiling;

    K_Map_SectorHeights(sector, &floor, &ceiling);
    return K_Graph_IsDoor(sector) || K_Graph_IsLift(sector) || ceiling - floor < PLAYER_HEIGHT;
}

static int ComparePairs(const void *x, const void *y)
{
    const pair_t *p = x, *q = y;

    return p->a != q->a ? p->a - q->a : p->b - q->b;
}

// Whether a crossing leaves the floor on both sides one: open, or only a
// solid thing standing over it; or too narrow for the player (the BSP's
// slivers: half the portals of Doom II MAP04 - the floor goes on round
// them), which within a sector joins, and between sectors only touches.
static boolean Continues(k_crossing_t crossing)
{
    return crossing == K_CROSS_OPEN || crossing == K_CROSS_THING
        || crossing == K_CROSS_NARROW;
}

// Whether the floor on both sides of the portal goes on, as the level
// stands at load (Continues), both ways, within a step.
static boolean OpenBothWays(const k_graph_t *graph, int portal)
{
    const k_portal_t *p = &graph->portals[portal];

    return p->jump < 0 && p->teleport_from < 0
        && Continues(K_Graph_CanCross(portal, p->cell[0]))
        && Continues(K_Graph_CanCross(portal, p->cell[1]))
        && fabs(K_Map_CellFloor(p->cell[0]) - K_Map_CellFloor(p->cell[1])) <= MAX_STEP;
}

static double PortalLength(const k_portal_t *p)
{
    return sqrt((p->b.x - p->a.x) * (p->b.x - p->a.x) + (p->b.y - p->a.y) * (p->b.y - p->a.y));
}

// What each portal is to the areas: PARTS - the floor parts there, or one
// side is a door or lift; TOUCHES - the floor goes on, but narrowly: too
// narrow to cross between two sectors, or two sectors meeting over less
// than NARROW of open border all told; JOINS - one room.
enum { PARTS, TOUCHES, JOINS };

static unsigned char *JoiningPortals(const k_map_t *map, const k_graph_t *graph)
{
    unsigned char *joins = calloc(SDL_max(graph->num_portals, 1), 1);
    pair_t *pairs = malloc(SDL_max(graph->num_portals, 1) * sizeof(*pairs));
    int num_pairs = 0, merged = 0;
    int i;

    for (i = 0; i < graph->num_portals; ++i)
    {
        const k_portal_t *p = &graph->portals[i];
        int sa, sb;

        if (!OpenBothWays(graph, i))
        {
            continue;
        }

        sa = CellSector(map, p->cell[0]);
        sb = CellSector(map, p->cell[1]);

        if (Passage(sa) || Passage(sb))
        {
            // A door's or lift's cells join one another only.
            joins[i] = sa == sb ? JOINS : PARTS;
            continue;
        }

        if (sa != sb && (K_Graph_CanCross(i, p->cell[0]) == K_CROSS_NARROW
                         || K_Graph_CanCross(i, p->cell[1]) == K_CROSS_NARROW))
        {
            joins[i] = TOUCHES;
            continue;
        }

        joins[i] = JOINS;

        if (sa != sb)
        {
            pairs[num_pairs].a = SDL_min(sa, sb);
            pairs[num_pairs].b = SDL_max(sa, sb);
            pairs[num_pairs++].length = PortalLength(p);
        }
    }

    // Each pair of sectors once, its open border added up.
    qsort(pairs, num_pairs, sizeof(*pairs), ComparePairs);

    for (i = 0; i < num_pairs; ++i)
    {
        if (merged > 0 && !ComparePairs(&pairs[merged - 1], &pairs[i]))
        {
            pairs[merged - 1].length += pairs[i].length;
        }
        else
        {
            pairs[merged++] = pairs[i];
        }
    }

    for (i = 0; i < graph->num_portals; ++i)
    {
        const k_portal_t *p = &graph->portals[i];
        pair_t key, *found;
        int sa, sb;

        if (joins[i] != JOINS)
        {
            continue;
        }

        sa = CellSector(map, p->cell[0]);
        sb = CellSector(map, p->cell[1]);

        if (sa == sb)
        {
            continue;
        }

        key.a = SDL_min(sa, sb);
        key.b = SDL_max(sa, sb);
        found = bsearch(&key, pairs, merged, sizeof(*pairs), ComparePairs);
        joins[i] = found != NULL && found->length >= NARROW ? JOINS : TOUCHES;
    }

    free(pairs);
    return joins;
}

static int CompareSouthWest(const void *x, const void *y)
{
    const k_map_t *map = K_Map_Get();
    const k_cell_t *p = &map->cells[*(const int *) x], *q = &map->cells[*(const int *) y];
    double a = p->centroid.x + p->centroid.y, b = q->centroid.x + q->centroid.y;

    return a < b ? -1 : a > b ? 1 : 0;
}

static boolean WithinSpan(k_vec2_t min, k_vec2_t max, const k_cell_t *cell, double span)
{
    return SDL_max(max.x, cell->max.x) - SDL_min(min.x, cell->min.x) <= span
        && SDL_max(max.y, cell->max.y) - SDL_min(min.y, cell->min.y) <= span;
}

// Grows the areas: from the cell furthest south-west left, nearest cells
// first (breadth first), while within SPAN. Returns how many.
static int Grow(const k_map_t *map, const k_graph_t *graph, const unsigned char *joins,
                int *area_of)
{
    int *order = malloc(SDL_max(map->num_cells, 1) * sizeof(*order));
    int *queue = malloc(SDL_max(map->num_cells, 1) * sizeof(*queue));
    int count = 0, i, k;

    for (i = 0; i < map->num_cells; ++i)
    {
        order[i] = i;
        area_of[i] = -1;
    }

    qsort(order, map->num_cells, sizeof(*order), CompareSouthWest);

    for (i = 0; i < map->num_cells; ++i)
    {
        int seed = order[i], head = 0, tail = 0;
        k_vec2_t min, max;

        if (area_of[seed] >= 0)
        {
            continue;
        }

        min = map->cells[seed].min;
        max = map->cells[seed].max;
        area_of[seed] = count;
        queue[tail++] = seed;

        while (head < tail)
        {
            int cell = queue[head++];

            for (k = 0; k < graph->link_count[cell]; ++k)
            {
                const k_link_t *link = &graph->links[graph->first_link[cell] + k];
                const k_cell_t *next = &map->cells[link->neighbor];

                if (joins[link->portal] != JOINS || area_of[link->neighbor] >= 0
                 || !WithinSpan(min, max, next, SPAN))
                {
                    continue;
                }

                area_of[link->neighbor] = count;
                queue[tail++] = link->neighbor;
                min.x = SDL_min(min.x, next->min.x);
                min.y = SDL_min(min.y, next->min.y);
                max.x = SDL_max(max.x, next->max.x);
                max.y = SDL_max(max.y, next->max.y);
            }
        }

        ++count;
    }

    free(queue);
    free(order);
    return count;
}

// Small areas join the neighbour they share most border with where the
// floor goes on (TOUCHES too: a light strip, a pillar's foot, a sliver
// between sectors is no room), within MERGE_SPAN; not passages.
static void MergeSmall(const k_map_t *map, const k_graph_t *graph, const unsigned char *joins,
                       int *area_of, int count)
{
    k_vec2_t *min = malloc(SDL_max(count, 1) * sizeof(*min));
    k_vec2_t *max = malloc(SDL_max(count, 1) * sizeof(*max));
    double *floor = calloc(SDL_max(count, 1), sizeof(*floor));
    double *shared = calloc(SDL_max(count, 1), sizeof(*shared));
    boolean *passage = calloc(SDL_max(count, 1), sizeof(*passage));
    int a, i, k;

    for (a = 0; a < count; ++a)
    {
        min[a].x = min[a].y = 1.0e9;
        max[a].x = max[a].y = -1.0e9;
    }

    for (i = 0; i < map->num_cells; ++i)
    {
        const k_cell_t *cell = &map->cells[i];

        a = area_of[i];
        min[a].x = SDL_min(min[a].x, cell->min.x);
        min[a].y = SDL_min(min[a].y, cell->min.y);
        max[a].x = SDL_max(max[a].x, cell->max.x);
        max[a].y = SDL_max(max[a].y, cell->max.y);
        floor[a] += cell->area;
        passage[a] |= Passage(CellSector(map, i));
    }

    for (a = 0; a < count; ++a)
    {
        int best = -1;

        if (floor[a] >= SMALL || passage[a])
        {
            continue;
        }

        memset(shared, 0, count * sizeof(*shared));

        for (i = 0; i < map->num_cells; ++i)
        {
            if (area_of[i] != a)
            {
                continue;
            }

            for (k = 0; k < graph->link_count[i]; ++k)
            {
                const k_link_t *link = &graph->links[graph->first_link[i] + k];
                int other = area_of[link->neighbor];

                if (joins[link->portal] != PARTS && other != a && !passage[other]
                 && SDL_max(max[a].x, max[other].x) - SDL_min(min[a].x, min[other].x) <= MERGE_SPAN
                 && SDL_max(max[a].y, max[other].y) - SDL_min(min[a].y, min[other].y) <= MERGE_SPAN)
                {
                    shared[other] += PortalLength(&graph->portals[link->portal]);

                    if (best < 0 || shared[other] > shared[best])
                    {
                        best = other;
                    }
                }
            }
        }

        if (best < 0)
        {
            continue;
        }

        for (i = 0; i < map->num_cells; ++i)
        {
            if (area_of[i] == a)
            {
                area_of[i] = best;
            }
        }

        min[best].x = SDL_min(min[best].x, min[a].x);
        min[best].y = SDL_min(min[best].y, min[a].y);
        max[best].x = SDL_max(max[best].x, max[a].x);
        max[best].y = SDL_max(max[best].y, max[a].y);
        floor[best] += floor[a];
        floor[a] = 0.0;
    }

    free(passage);
    free(shared);
    free(floor);
    free(max);
    free(min);
}

// Up to SAMPLES cells spread over the area: its largest, then each time the
// one furthest from those chosen.
static void ChooseSamples(const k_map_t *map, int a)
{
    const k_area_t *area = &areamap.areas[a];
    int s, i;

    num_samples[a] = 0;

    for (s = 0; s < SAMPLES && s < area->num_cells; ++s)
    {
        int best = -1;
        double best_score = -1.0;

        for (i = 0; i < area->num_cells; ++i)
        {
            int cell = areamap.cells[area->first_cell + i];
            const k_cell_t *c = &map->cells[cell];
            double nearest = 1.0e9;
            int k;

            for (k = 0; k < num_samples[a]; ++k)
            {
                const k_cell_t *o = &map->cells[samples[a][k]];
                double dx = c->centroid.x - o->centroid.x, dy = c->centroid.y - o->centroid.y;

                nearest = SDL_min(nearest, sqrt(dx * dx + dy * dy));
            }

            if (num_samples[a] == 0 ? c->area > best_score
                                    : nearest > 64.0 && nearest > best_score)
            {
                best = cell;
                best_score = num_samples[a] == 0 ? c->area : nearest;
            }
        }

        if (best < 0)
        {
            break;
        }

        samples[a][num_samples[a]++] = best;
    }
}

void K_Area_Build(void)
{
    const k_map_t *map = K_Map_Get();
    const k_graph_t *graph = K_Graph_Get();
    Uint64 start = SDL_GetPerformanceCounter();
    unsigned char *joins;
    int *renumber, count, a, i;

    areamap.num_areas = 0;
    areamap.passages = 0;
    next_refresh = 0;

    if (cell_capacity < map->num_cells)
    {
        cell_capacity = map->num_cells;
        areamap.cells = realloc(areamap.cells, cell_capacity * sizeof(*areamap.cells));
        areamap.area_of = realloc(areamap.area_of, cell_capacity * sizeof(*areamap.area_of));
    }

    if (map->num_cells == 0)
    {
        areamap.generation = map->generation;
        num_portal_joins = 0;
        return;
    }

    joins = JoiningPortals(map, graph);
    count = Grow(map, graph, joins, areamap.area_of);
    MergeSmall(map, graph, joins, areamap.area_of, count);
    free(portal_joins);
    portal_joins = joins;
    num_portal_joins = graph->num_portals;

    // Numbered anew without the areas merged away, in the order of their
    // first cells.
    renumber = malloc(count * sizeof(*renumber));

    for (a = 0; a < count; ++a)
    {
        renumber[a] = -1;
    }

    for (i = 0; i < map->num_cells; ++i)
    {
        a = areamap.area_of[i];

        if (renumber[a] < 0)
        {
            renumber[a] = areamap.num_areas++;
        }

        areamap.area_of[i] = renumber[a];
    }

    free(renumber);

    if (area_capacity < areamap.num_areas)
    {
        area_capacity = areamap.num_areas;
        areamap.areas = realloc(areamap.areas, area_capacity * sizeof(*areamap.areas));
        samples = realloc(samples, area_capacity * sizeof(*samples));
        num_samples = realloc(num_samples, area_capacity * sizeof(*num_samples));
    }

    memset(areamap.areas, 0, areamap.num_areas * sizeof(*areamap.areas));

    // Each area's cells, back to back.
    for (i = 0; i < map->num_cells; ++i)
    {
        ++areamap.areas[areamap.area_of[i]].num_cells;
    }

    for (a = 0, count = 0; a < areamap.num_areas; ++a)
    {
        areamap.areas[a].first_cell = count;
        count += areamap.areas[a].num_cells;
        areamap.areas[a].num_cells = 0;
        areamap.areas[a].min.x = areamap.areas[a].min.y = 1.0e9;
        areamap.areas[a].max.x = areamap.areas[a].max.y = -1.0e9;
        areamap.areas[a].passage = true;
    }

    for (i = 0; i < map->num_cells; ++i)
    {
        const k_cell_t *cell = &map->cells[i];
        k_area_t *area = &areamap.areas[areamap.area_of[i]];

        areamap.cells[area->first_cell + area->num_cells++] = i;
        area->min.x = SDL_min(area->min.x, cell->min.x);
        area->min.y = SDL_min(area->min.y, cell->min.y);
        area->max.x = SDL_max(area->max.x, cell->max.x);
        area->max.y = SDL_max(area->max.y, cell->max.y);
        area->center.x += cell->centroid.x * cell->area;
        area->center.y += cell->centroid.y * cell->area;
        area->floor_area += cell->area;
        area->passage &= Passage(CellSector(map, i));
    }

    for (a = 0; a < areamap.num_areas; ++a)
    {
        k_area_t *area = &areamap.areas[a];

        if (area->floor_area > 0.0)
        {
            area->center.x /= area->floor_area;
            area->center.y /= area->floor_area;
        }

        areamap.passages += area->passage;
        ChooseSamples(map, a);
        area->spot = num_samples[a] > 0 ? map->cells[samples[a][0]].centroid : area->center;
    }

    areamap.generation = map->generation;
    areamap.build_ms = (SDL_GetPerformanceCounter() - start) * 1000.0
                     / SDL_GetPerformanceFrequency();
    K_Log(K_LOG_MAP, K_LOG_INFO, "%s areas: %d (%d of them doors or lifts) from %d cells, "
          "%.1f ms", map->name, areamap.num_areas, areamap.passages, map->num_cells,
          areamap.build_ms);
}

// The distance from a point to an area's bounding box.
static double BoxDistance(const k_area_t *area, k_vec2_t at)
{
    double dx = SDL_max(0.0, SDL_max(area->min.x - at.x, at.x - area->max.x));
    double dy = SDL_max(0.0, SDL_max(area->min.y - at.y, at.y - area->max.y));

    return sqrt(dx * dx + dy * dy);
}

// Its danger, and the share of its floor that hurts.
static void Refresh(int a)
{
    const k_map_t *map = K_Map_Get();
    k_area_t *area = &areamap.areas[a];
    double hurting = 0.0;
    int i, k;

    area->danger = 0.0;
    area->awake_danger = 0.0;

    for (i = 0; i < K_Combat_MonsterCount(); ++i)
    {
        const k_monster_t *monster = K_Combat_Monster(i);
        double cost = K_Combat_KindCost(monster->type);
        double distance = BoxDistance(area, monster->at);
        boolean sees = monster_area[i] == a;

        if (cost <= 0.0 || distance > DANGER_REACH)
        {
            continue;
        }

        for (k = 0; !sees && k < num_samples[a]; ++k)
        {
            const k_cell_t *sample = &map->cells[samples[a][k]];
            double floor = K_Map_CellFloor(samples[a][k]);

            sees = K_Sight_Check(monster->at.x, monster->at.y,
                                 monster->z + monster->height * 0.75, sample->centroid.x,
                                 sample->centroid.y, floor, floor + PLAYER_HEIGHT);
        }

        if (sees)
        {
            double share = distance <= DANGER_FULL
                         ? 1.0 : (DANGER_REACH - distance) / (DANGER_REACH - DANGER_FULL);

            area->danger += cost * share * (monster->awake ? 1.0 : ASLEEP_SHARE);
            area->awake_danger += monster->awake ? cost * share : 0.0;
        }
    }

    for (i = 0; i < area->num_cells; ++i)
    {
        int cell = areamap.cells[area->first_cell + i];

        if (K_Map_FloorDamage(CellSector(map, cell)) > 0.0)
        {
            hurting += map->cells[cell].area;
        }
    }

    area->hurts = area->floor_area > 0.0 ? hurting / area->floor_area : 0.0;
}

void K_Area_Tick(void)
{
    const k_map_t *map = K_Map_Get();
    k_pose_t player;
    int a, i, count;

    if (areamap.generation != map->generation || areamap.num_areas == 0)
    {
        return;
    }

    if (K_Map_GetPlayer(&player) && player.cell >= 0)
    {
        areamap.areas[areamap.area_of[player.cell]].visited = true;
    }

    // The monsters inside each.
    for (a = 0; a < areamap.num_areas; ++a)
    {
        areamap.areas[a].monsters = 0;
        areamap.areas[a].awake = 0;
        areamap.areas[a].inside = 0.0;
    }

    if (monster_capacity < K_Combat_MonsterCount())
    {
        monster_capacity = K_Combat_MonsterCount() * 2;
        monster_area = realloc(monster_area, monster_capacity * sizeof(*monster_area));
    }

    for (i = 0; i < K_Combat_MonsterCount(); ++i)
    {
        const k_monster_t *monster = K_Combat_Monster(i);
        int cell = K_Map_CellAt(monster->at.x, monster->at.y);
        k_area_t *area;

        monster_area[i] = cell >= 0 ? areamap.area_of[cell] : -1;

        if (cell < 0 || K_Combat_KindCost(monster->type) <= 0.0)
        {
            continue;
        }

        area = &areamap.areas[monster_area[i]];
        ++area->monsters;
        area->awake += monster->awake;
        area->inside += K_Combat_KindCost(monster->type) * (monster->awake ? 1.0 : ASLEEP_SHARE);
    }

    // A few dangers a tic.
    count = (areamap.num_areas + REFRESH_TICS - 1) / REFRESH_TICS;

    for (i = 0; i < count; ++i)
    {
        Refresh(next_refresh);
        next_refresh = (next_refresh + 1) % areamap.num_areas;
    }
}

const k_areamap_t *K_Area_Get(void)
{
    return &areamap;
}

int K_Area_Of(int cell)
{
    return areamap.generation == K_Map_Get()->generation && cell >= 0
        && cell < K_Map_Get()->num_cells ? areamap.area_of[cell] : -1;
}

int K_Area_OpenFloor(int portal)
{
    return areamap.generation == K_Map_Get()->generation && portal >= 0
        && portal < num_portal_joins && portal_joins[portal] == JOINS;
}

int K_Area_Safe(int area)
{
    return area >= 0 && area < areamap.num_areas && areamap.areas[area].monsters == 0
        && areamap.areas[area].danger < SAFE_DANGER;
}
