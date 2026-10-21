//
// DESCRIPTION:
//	Karp map reader. Built from the engine's level data at the end of
//	P_SetupLevel. All level memory is PU_LEVEL and is freed on the next
//	load, so Karp keeps its own copies and rebuilds them every time.
//
//	Sector heights are always read live from the engine: floors move, and
//	loading a savegame changes them after P_SetupLevel.
//
//	Subsector polygons: in vanilla, DeePBSP and XNOD nodes a subsector's
//	segs only cover its linedef edges, so its outline is open wherever it
//	borders another subsector along a partition line. The closed polygon
//	is rebuilt by clipping the map's bounding box down the BSP tree with
//	each partition line, then by each of the subsector's own segs, which
//	trims the void behind one-sided walls. This works the same for every
//	node format.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomstat.h"
#include "p_mapformat.h"
#include "r_main.h"
#include "r_state.h"
#include "w_wad.h"
#include "p_setup.h"

#include "k_log.h"
#include "k_map.h"

#define K_PI 3.14159265358979323846

// Points this close to a clipping line count as on it.
#define CLIP_EPSILON (1.0 / 256)

// Margin around the map for the starting polygon.
#define BOX_PADDING 64.0

// Vanilla nodebuilders round split vertices to whole map units, so
// rebuilt polygons can leave hairline gaps; point lookups allow for that.
#define INSIDE_TOLERANCE 1.0

// Below this area, or this average width, a polygon has no usable floor:
// nothing can stand in it (the player is 32 units wide). This covers the
// hairline slivers that vertex rounding leaves along lines.
#define MIN_AREA 0.01
#define MIN_WIDTH 1.0

// Per kind of problem, only this many are logged individually.
#define MAX_REPORTS 5

static k_map_t map;
static int points_capacity;
static unsigned char *reached;

static double FixedToMap(fixed_t value)
{
    return (double) value / FRACUNIT;
}

static fixed_t MapToFixed(double value)
{
    return (fixed_t) (value * FRACUNIT);
}

static k_vec2_t VertexToMap(const vertex_t *vertex)
{
    k_vec2_t result;

    result.x = FixedToMap(vertex->x);
    result.y = FixedToMap(vertex->y);
    return result;
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

static void BuildLines(void)
{
    int i;

    free(map.lines);
    map.lines = malloc(SDL_max(numlines, 1) * sizeof(*map.lines));
    map.num_lines = numlines;

    map.min.x = map.min.y = 0.0;
    map.max.x = map.max.y = 0.0;

    for (i = 0; i < numlines; ++i)
    {
        const line_t *source = &lines[i];
        k_line_t *line = &map.lines[i];

        line->v1 = VertexToMap(source->v1);
        line->v2 = VertexToMap(source->v2);
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
}

//
// Polygon helpers
//

// Signed distance of a point from the directed line through origin along
// dir: negative on the right (the engine's "front"), positive on the left.
static double LineSide(k_vec2_t origin, k_vec2_t dir, k_vec2_t point)
{
    return (dir.x * (point.y - origin.y) - dir.y * (point.x - origin.x))
         / sqrt(dir.x * dir.x + dir.y * dir.y);
}

// Clips a convex polygon to one side of a line, keeping points on the
// line. Returns the new point count; out must have room for n + 2 points.
static int ClipPolygon(const k_vec2_t *in, int n, k_vec2_t *out,
                       k_vec2_t origin, k_vec2_t dir, boolean keep_right)
{
    double sign = keep_right ? -1.0 : 1.0;
    int count = 0;
    int i;

    for (i = 0; i < n; ++i)
    {
        k_vec2_t a = in[i];
        k_vec2_t b = in[(i + 1) % n];
        double side_a = sign * LineSide(origin, dir, a);
        double side_b = sign * LineSide(origin, dir, b);

        if (side_a >= -CLIP_EPSILON)
        {
            out[count++] = a;
        }

        // The edge crosses the line: keep the crossing point.
        if ((side_a > CLIP_EPSILON && side_b < -CLIP_EPSILON)
         || (side_a < -CLIP_EPSILON && side_b > CLIP_EPSILON))
        {
            double t = side_a / (side_a - side_b);

            out[count].x = a.x + t * (b.x - a.x);
            out[count].y = a.y + t * (b.y - a.y);
            ++count;
        }
    }

    return count;
}

// Drops repeated points and points lying on a straight edge, which
// clipping leaves behind. Returns the new point count.
static int CleanPolygon(k_vec2_t *poly, int n)
{
    int i = 0;

    while (n >= 3 && i < n)
    {
        k_vec2_t prev = poly[(i + n - 1) % n];
        k_vec2_t next = poly[(i + 1) % n];
        k_vec2_t dir = { next.x - prev.x, next.y - prev.y };
        double length = sqrt(dir.x * dir.x + dir.y * dir.y);

        if (length < CLIP_EPSILON
         || fabs(LineSide(prev, dir, poly[i])) < CLIP_EPSILON)
        {
            memmove(&poly[i], &poly[i + 1], (n - i - 1) * sizeof(*poly));
            --n;

            // Removing a point can make its neighbour removable too.
            i = SDL_max(i - 1, 0);
        }
        else
        {
            ++i;
        }
    }

    return n;
}

// Shoelace formula, relative to the first point to keep precision with
// large map coordinates.
static double PolygonArea(const k_vec2_t *poly, int n, k_vec2_t *centroid)
{
    double area = 0.0, cx = 0.0, cy = 0.0;
    int i;

    for (i = 0; i < n; ++i)
    {
        double ax = poly[i].x - poly[0].x;
        double ay = poly[i].y - poly[0].y;
        double bx = poly[(i + 1) % n].x - poly[0].x;
        double by = poly[(i + 1) % n].y - poly[0].y;
        double cross = ax * by - bx * ay;

        area += cross;
        cx += (ax + bx) * cross;
        cy += (ay + by) * cross;
    }

    area *= 0.5;

    if (centroid != NULL && fabs(area) > 0.0)
    {
        centroid->x = poly[0].x + cx / (6.0 * area);
        centroid->y = poly[0].y + cy / (6.0 * area);
    }

    return area;
}

static boolean HasNoArea(const k_vec2_t *poly, int n)
{
    double area, perimeter = 0.0;
    int i;

    if (n < 3)
    {
        return true;
    }

    for (i = 0; i < n; ++i)
    {
        k_vec2_t a = poly[i];
        k_vec2_t b = poly[(i + 1) % n];

        perimeter += sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    }

    // Area over half the perimeter is roughly the average width.
    area = PolygonArea(poly, n, NULL);
    return area < MIN_AREA || area / (perimeter * 0.5) < MIN_WIDTH;
}

static void StorePolygon(k_subsector_t *ss, const k_vec2_t *poly, int n)
{
    int i;

    if (map.num_points + n > points_capacity)
    {
        points_capacity = SDL_max(points_capacity * 2, map.num_points + n + 1024);
        map.points = realloc(map.points, points_capacity * sizeof(*map.points));
    }

    ss->first_point = map.num_points;
    ss->num_points = n;
    memcpy(&map.points[map.num_points], poly, n * sizeof(*poly));
    map.num_points += n;

    if (n == 0)
    {
        return;
    }

    ss->min = ss->max = poly[0];

    for (i = 1; i < n; ++i)
    {
        ss->min.x = SDL_min(ss->min.x, poly[i].x);
        ss->min.y = SDL_min(ss->min.y, poly[i].y);
        ss->max.x = SDL_max(ss->max.x, poly[i].x);
        ss->max.y = SDL_max(ss->max.y, poly[i].y);
    }

    ss->area = n >= 3 ? PolygonArea(poly, n, &ss->centroid) : 0.0;
}

static boolean PointInPolygon(const k_subsector_t *ss, double x, double y,
                              double tolerance)
{
    const k_vec2_t *poly = &map.points[ss->first_point];
    k_vec2_t point = { x, y };
    int i;

    if (ss->num_points < 3
     || x < ss->min.x - tolerance || x > ss->max.x + tolerance
     || y < ss->min.y - tolerance || y > ss->max.y + tolerance)
    {
        return false;
    }

    // Counter-clockwise: the inside is on the left of every edge.
    for (i = 0; i < ss->num_points; ++i)
    {
        k_vec2_t a = poly[i];
        k_vec2_t b = poly[(i + 1) % ss->num_points];
        k_vec2_t dir = { b.x - a.x, b.y - a.y };

        if (LineSide(a, dir, point) < -tolerance)
        {
            return false;
        }
    }

    return true;
}

//
// Subsector polygons
//

// For subsectors without a polygon: a centroid among the segs, for display.
static void PlaceAmongSegs(k_subsector_t *ss, const subsector_t *source)
{
    int i;

    ss->centroid.x = ss->centroid.y = 0.0;

    for (i = 0; i < source->numlines; ++i)
    {
        k_vec2_t v1 = VertexToMap(segs[source->firstline + i].v1);

        ss->centroid.x += v1.x / source->numlines;
        ss->centroid.y += v1.y / source->numlines;
    }
}

// Clips the polygon in work by the subsector's segs (or only its one-sided
// ones), keeping the right side of each: the subsector lies there. temp is
// scratch. Both need room for count + 2 * numlines + 2 points, as each
// clip adds at most two. Returns the new point count.
static int ClipBySegs(const subsector_t *source, k_vec2_t *work,
                      k_vec2_t *temp, int count, boolean one_sided_only)
{
    int i;

    for (i = 0; i < source->numlines && count >= 3; ++i)
    {
        const seg_t *seg = &segs[source->firstline + i];
        k_vec2_t v1 = VertexToMap(seg->v1);
        k_vec2_t v2 = VertexToMap(seg->v2);
        k_vec2_t dir = { v2.x - v1.x, v2.y - v1.y };

        if ((one_sided_only && seg->backsector != NULL)
         || sqrt(dir.x * dir.x + dir.y * dir.y) < CLIP_EPSILON)
        {
            continue;
        }

        count = ClipPolygon(work, count, temp, v1, dir, true);
        memcpy(work, temp, count * sizeof(*work));
    }

    return CleanPolygon(work, count);
}

static void BuildLeaf(int index, const k_vec2_t *poly, int n)
{
    const subsector_t *source;
    k_subsector_t *ss;
    k_vec2_t *work, *temp;
    int count;

    if (index < 0 || index >= numsubsectors || reached[index])
    {
        return;
    }

    reached[index] = 1;
    source = &subsectors[index];
    ss = &map.subsectors[index];
    ss->sector = SectorIndex(source->sector);

    // Leaves with no usable floor need no polygon: some nodebuilders split
    // along the same line twice, leaving a leaf with segs but no area, and
    // vertex rounding leaves hairline slivers along lines.
    if (HasNoArea(poly, n))
    {
        ss->no_area = 1;
        StorePolygon(ss, poly, 0);
        PlaceAmongSegs(ss, source);
        return;
    }

    work = malloc((n + 2 * source->numlines + 2) * sizeof(*work));
    temp = malloc((n + 2 * source->numlines + 2) * sizeof(*temp));
    memcpy(work, poly, n * sizeof(*poly));
    count = ClipBySegs(source, work, temp, n, false);

    // The segs left no usable floor. Find out which ones remove it:
    //  - its one-sided walls: the leaf is solid space behind them, e.g.
    //    inside a pillar. Nodebuilders leave these behind thin walls.
    //    Harmless, no floor.
    //  - a two-sided seg (buggy nodes): the floor lies on that seg's far
    //    side, yet the engine puts it in this subsector. Keep the BSP
    //    region minus what lies behind the walls.
    if (HasNoArea(work, count))
    {
        memcpy(work, poly, n * sizeof(*poly));
        count = ClipBySegs(source, work, temp, n, true);

        if (HasNoArea(work, count))
        {
            ss->behind_wall = 1;
            count = 0;
        }
        else
        {
            ss->degenerate = 1;
        }
    }

    StorePolygon(ss, work, count);

    if (count == 0)
    {
        PlaceAmongSegs(ss, source);
    }

    free(work);
    free(temp);
}

static void BuildNode(int child, const k_vec2_t *poly, int n)
{
    const node_t *node;
    k_vec2_t origin, dir;
    k_vec2_t *front, *back;
    int num_front, num_back;

    if (child & NF_SUBSECTOR)
    {
        BuildLeaf(child & ~NF_SUBSECTOR, poly, n);
        return;
    }

    if (child < 0 || child >= numnodes)
    {
        return;
    }

    node = &nodes[child];
    origin.x = FixedToMap(node->x);
    origin.y = FixedToMap(node->y);
    dir.x = FixedToMap(node->dx);
    dir.y = FixedToMap(node->dy);

    // R_PointOnSide puts everything on the front of a zero-length partition.
    if (dir.x == 0.0 && dir.y == 0.0)
    {
        BuildNode(node->children[0], poly, n);
        BuildNode(node->children[1], poly, 0);
        return;
    }

    front = malloc((n + 2) * sizeof(*front));
    back = malloc((n + 2) * sizeof(*back));

    num_front = ClipPolygon(poly, n, front, origin, dir, true);
    num_back = ClipPolygon(poly, n, back, origin, dir, false);

    BuildNode(node->children[0], front, num_front);
    BuildNode(node->children[1], back, num_back);

    free(front);
    free(back);
}

static void BuildSubsectors(void)
{
    // Counter-clockwise, which every clipped polygon inherits.
    k_vec2_t box[4] =
    {
        { map.min.x - BOX_PADDING, map.min.y - BOX_PADDING },
        { map.max.x + BOX_PADDING, map.min.y - BOX_PADDING },
        { map.max.x + BOX_PADDING, map.max.y + BOX_PADDING },
        { map.min.x - BOX_PADDING, map.max.y + BOX_PADDING },
    };

    free(map.subsectors);
    map.subsectors = calloc(SDL_max(numsubsectors, 1), sizeof(*map.subsectors));
    map.num_subsectors = numsubsectors;
    map.num_points = 0;
    map.num_nodes = numnodes;

    reached = calloc(SDL_max(numsubsectors, 1), 1);

    // A map with a single subsector has no nodes at all.
    if (numnodes == 0)
    {
        BuildLeaf(0, box, 4);
    }
    else
    {
        BuildNode(numnodes - 1, box, 4);
    }
}

static void CheckSubsectors(void)
{
    int i;

    map.no_area_subsectors = 0;
    map.behind_wall_subsectors = 0;
    map.degenerate_subsectors = 0;
    map.unreached_subsectors = 0;
    map.centroid_mismatches = 0;

    for (i = 0; i < map.num_subsectors; ++i)
    {
        k_subsector_t *ss = &map.subsectors[i];

        if (!reached[i])
        {
            if (map.unreached_subsectors++ < MAX_REPORTS)
            {
                K_Log(K_LOG_MAP, K_LOG_WARN, "subsector %d is not reachable "
                      "through the BSP tree", i);
            }

            continue;
        }

        if (ss->no_area)
        {
            ++map.no_area_subsectors;
            continue;
        }

        if (ss->behind_wall)
        {
            ++map.behind_wall_subsectors;
            continue;
        }

        if (ss->degenerate)
        {
            if (map.degenerate_subsectors++ < MAX_REPORTS)
            {
                K_Log(K_LOG_MAP, K_LOG_WARN, "subsector %d at (%.0f,%.0f): its "
                      "floor lies behind one of its two-sided segs - using "
                      "its BSP region", i, ss->centroid.x, ss->centroid.y);
            }
        }

        if (ss->num_points >= 3)
        {
            int found = (int) (R_PointInSubsector(MapToFixed(ss->centroid.x),
                                                  MapToFixed(ss->centroid.y))
                               - subsectors);

            if (found != i)
            {
                ss->centroid_mismatch = 1;

                if (map.centroid_mismatches++ < MAX_REPORTS)
                {
                    K_Log(K_LOG_MAP, K_LOG_WARN, "subsector %d: centroid "
                          "(%.0f,%.0f) lies in subsector %d", i,
                          ss->centroid.x, ss->centroid.y, found);
                }
            }
        }
    }

    free(reached);
    reached = NULL;
}

void K_Map_Build(void)
{
    Uint64 start = SDL_GetPerformanceCounter();
    int problems;

    map.name[0] = '\0';

    if (maplumpinfo != NULL)
    {
        // Lump names are not null-terminated when all 8 characters are used.
        memcpy(map.name, maplumpinfo->name, 8);
        map.name[8] = '\0';
    }

    map.num_sectors = numsectors;
    BuildLines();
    BuildSubsectors();
    CheckSubsectors();

    map.loaded = 1;
    ++map.generation;
    map.build_ms = (SDL_GetPerformanceCounter() - start) * 1000.0
                 / SDL_GetPerformanceFrequency();

    problems = map.degenerate_subsectors + map.unreached_subsectors
             + map.centroid_mismatches;

    K_Log(K_LOG_MAP, problems > 0 ? K_LOG_WARN : K_LOG_INFO,
          "%s: %d lines, %d sectors, %d subsectors (%d without area, %d behind "
          "walls) - %d degenerate, %d unreached, %d centroid mismatches, "
          "%.1f ms", map.name, map.num_lines, map.num_sectors,
          map.num_subsectors, map.no_area_subsectors,
          map.behind_wall_subsectors, map.degenerate_subsectors,
          map.unreached_subsectors, map.centroid_mismatches, map.build_ms);
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

int K_Map_SubsectorAt(double x, double y)
{
    int index;

    if (!map.loaded || map.num_subsectors == 0)
    {
        return -1;
    }

    // The BSP walk always lands in some subsector, even from the void.
    index = (int) (R_PointInSubsector(MapToFixed(x), MapToFixed(y)) - subsectors);

    if (index < 0 || index >= map.num_subsectors
     || !PointInPolygon(&map.subsectors[index], x, y, INSIDE_TOLERANCE))
    {
        return -1;
    }

    return index;
}

void K_Map_SectorHeights(int sector, double *floor, double *ceiling)
{
    if (sector < 0 || sector >= numsectors)
    {
        *floor = *ceiling = 0.0;
        return;
    }

    *floor = FixedToMap(sectors[sector].floorheight);
    *ceiling = FixedToMap(sectors[sector].ceilingheight);
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
