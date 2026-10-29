//
// DESCRIPTION:
//	Karp map reader. Built from the engine's level data at the end of
//	P_SetupLevel. All level memory is PU_LEVEL and is freed on the next
//	load, so Karp keeps its own copies and rebuilds them every time.
//
//	Sector heights are always read live from the engine: floors move, and
//	loading a savegame changes them after P_SetupLevel.
//
//	Floor cells: in vanilla, DeePBSP and XNOD nodes a subsector's segs only
//	cover its linedef edges, so its outline is open wherever it borders
//	another subsector along a partition line. Each subsector's BSP region
//	is rebuilt by clipping the map's bounding box down the BSP tree with
//	each partition line. That region is then split by every linedef that
//	passes through it into convex pieces, and each piece is kept as a floor
//	cell if it lies in a sector, or dropped as void. This works the same
//	for every node format.
//
//	A subsector's segs are convex, but its floor need not be, and its
//	region need not stop at its own walls:
//	- a wall corner inside the region leaves floor beside the wall, past
//	  the end of its line;
//	- void is never visible, so nodebuilders give it to whichever leaf is
//	  convenient, beyond walls whose segs belong to other leaves.
//	Splitting by all lines through the region, then judging each piece on
//	its own, handles both.
//
//	Solid things that are not monsters (blocks) are cut out of the floor
//	cells like pillars, but the cells under them stay, judged live: a
//	barrel that blows up opens the way.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomstat.h"
#include "p_local.h"
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

// The player's radius (MT_PLAYER): its box is twice that wide.
#define PLAYER_RADIUS 16.0

// The highest step the player climbs (P_TryMove).
#define MAX_STEP_UP 24.0

// A block's thing still stands if it is at most this far from where it
// stood (a shot pushes a barrel a little); further off, its square no
// longer covers it.
#define BLOCK_DRIFT 4.0

// Blocks are bucketed in a uniform grid of this size.
#define BLOCK_GRID 128.0

// A convex polygon being split into cells.
typedef struct
{
    k_vec2_t *points;
    int count;
} piece_t;

static k_map_t map;
static int cells_capacity;
static int points_capacity;
static unsigned char *reached;

// Per block: its thing's type, and whether the thing still stands.
static mobjtype_t *block_types;
static unsigned char *block_standing;
static int blocks_judged;           // leveltime + 1 when last judged, 0 never

// Per cell, the blocks whose squares it lies under (mostly none).
static int *cell_block_first;       // into cell_block_list; one extra at the end
static int *cell_block_list;

// Grid of blocks, by their squares.
static k_vec2_t block_origin;
static int block_width, block_height;
static int *block_first;            // per grid cell, into block_items; one
                                    // extra at the end
static int *block_items;
static unsigned int *block_seen;    // per block, the query that last met it
static unsigned int block_query;

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

static void ExtendBounds(k_vec2_t *min, k_vec2_t *max, const k_vec2_t *point)
{
    min->x = SDL_min(min->x, point->x);
    min->y = SDL_min(min->y, point->y);
    max->x = SDL_max(max->x, point->x);
    max->y = SDL_max(max->y, point->y);
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

        ExtendBounds(&map.min, &map.max, &line->v1);
        ExtendBounds(&map.min, &map.max, &line->v2);
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

static boolean PointInCell(const k_cell_t *cell, double x, double y,
                           double tolerance)
{
    const k_vec2_t *poly = &map.points[cell->first_point];
    k_vec2_t point = { x, y };
    int i;

    if (x < cell->min.x - tolerance || x > cell->max.x + tolerance
     || y < cell->min.y - tolerance || y > cell->max.y + tolerance)
    {
        return false;
    }

    // Counter-clockwise: the inside is on the left of every edge.
    for (i = 0; i < cell->num_points; ++i)
    {
        k_vec2_t a = poly[i];
        k_vec2_t b = poly[(i + 1) % cell->num_points];
        k_vec2_t dir = { b.x - a.x, b.y - a.y };

        if (LineSide(a, dir, point) < -tolerance)
        {
            return false;
        }
    }

    return true;
}

static double Cross(k_vec2_t a, k_vec2_t b)
{
    return a.x * b.y - a.y * b.x;
}

// The sector a point is in, from the first line a ray from it hits: the
// point is on that line's front side (its right) or back side. -1 for
// void: a one-sided line seen from behind, or nothing hit at all. This is
// a local test, so unlike an even-odd test over sector outlines it is not
// fooled by the unclosed outlines vanilla maps have. Three rays vote, so a
// single gap between lines cannot fool it either.
static int SectorAtPoint(k_vec2_t point)
{
    // Not axis-aligned, so rays rarely pass exactly through vertices.
    static const double angles[3] = { 0.7, 2.8, 4.9 };
    int votes[3];
    int ray, i;

    for (ray = 0; ray < 3; ++ray)
    {
        k_vec2_t dir = { cos(angles[ray]), sin(angles[ray]) };
        double nearest = 1e30;
        int hit = -1;

        for (i = 0; i < map.num_lines; ++i)
        {
            const k_line_t *line = &map.lines[i];
            k_vec2_t along = { line->v2.x - line->v1.x, line->v2.y - line->v1.y };
            k_vec2_t to_line = { line->v1.x - point.x, line->v1.y - point.y };
            double denom = Cross(dir, along);
            double t, u;

            if (fabs(denom) < 1e-12)
            {
                continue;
            }

            t = Cross(to_line, along) / denom;     // distance along the ray
            u = Cross(to_line, dir) / denom;       // position along the line

            if (t > 1e-6 && t < nearest && u >= 0.0 && u <= 1.0)
            {
                nearest = t;
                hit = i;
            }
        }

        if (hit < 0)
        {
            votes[ray] = -1;
        }
        else
        {
            const k_line_t *line = &map.lines[hit];
            k_vec2_t along = { line->v2.x - line->v1.x, line->v2.y - line->v1.y };

            votes[ray] = LineSide(line->v1, along, point) < 0.0
                       ? line->front_sector : line->back_sector;
        }
    }

    if (votes[0] == votes[1] || votes[0] == votes[2])
    {
        return votes[0];
    }

    if (votes[1] == votes[2])
    {
        return votes[1];
    }

    // All three disagree: trust any floor over void.
    return votes[0] >= 0 ? votes[0] : votes[1] >= 0 ? votes[1] : votes[2];
}

// True if the segment a-b passes through the interior of a convex,
// counter-clockwise polygon (not just along its border).
static boolean SegmentCrossesPolygon(k_vec2_t a, k_vec2_t b,
                                     const k_vec2_t *poly, int n)
{
    k_vec2_t ab = { b.x - a.x, b.y - a.y };
    double t0 = 0.0, t1 = 1.0;
    int i;

    for (i = 0; i < n; ++i)
    {
        k_vec2_t p = poly[i];
        k_vec2_t dir = { poly[(i + 1) % n].x - p.x, poly[(i + 1) % n].y - p.y };
        // Positive inside: the polygon is on the left of its edges.
        double side_a = LineSide(p, dir, a) - CLIP_EPSILON;
        double side_b = LineSide(p, dir, b) - CLIP_EPSILON;

        if (side_a <= 0.0 && side_b <= 0.0)
        {
            return false;
        }

        if (side_a < 0.0)
        {
            t0 = SDL_max(t0, side_a / (side_a - side_b));
        }
        else if (side_b < 0.0)
        {
            t1 = SDL_min(t1, side_a / (side_a - side_b));
        }

        if (t0 >= t1)
        {
            return false;
        }
    }

    return (t1 - t0) * sqrt(ab.x * ab.x + ab.y * ab.y) > MIN_WIDTH;
}

//
// Blocks
//

static void BlockSquare(const k_block_t *block, k_vec2_t *min, k_vec2_t *max)
{
    min->x = block->at.x - block->radius;
    min->y = block->at.y - block->radius;
    max->x = block->at.x + block->radius;
    max->y = block->at.y + block->radius;
}

static void BlockRange(k_vec2_t min, k_vec2_t max, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = SDL_max(0, (int) floor((min.x - block_origin.x) / BLOCK_GRID));
    *y0 = SDL_max(0, (int) floor((min.y - block_origin.y) / BLOCK_GRID));
    *x1 = SDL_min(block_width - 1, (int) floor((max.x - block_origin.x) / BLOCK_GRID));
    *y1 = SDL_min(block_height - 1, (int) floor((max.y - block_origin.y) / BLOCK_GRID));
}

static void BuildBlockGrid(void)
{
    int num_cells, b, x, y, x0, y0, x1, y1, pass;
    k_vec2_t min, max;
    int *fill = NULL;

    // Things can stand a little past the lines.
    block_origin.x = map.min.x - BLOCK_GRID;
    block_origin.y = map.min.y - BLOCK_GRID;
    block_width = (int) ((map.max.x - block_origin.x) / BLOCK_GRID) + 2;
    block_height = (int) ((map.max.y - block_origin.y) / BLOCK_GRID) + 2;
    num_cells = block_width * block_height;

    free(block_first);
    free(block_items);
    block_first = calloc(num_cells + 1, sizeof(*block_first));
    block_items = NULL;

    // Count the blocks per grid cell, then fill.
    for (pass = 0; pass < 2; ++pass)
    {
        for (b = 0; b < map.num_blocks; ++b)
        {
            BlockSquare(&map.blocks[b], &min, &max);
            BlockRange(min, max, &x0, &y0, &x1, &y1);

            for (y = y0; y <= y1; ++y)
            {
                for (x = x0; x <= x1; ++x)
                {
                    if (pass == 0)
                    {
                        ++block_first[y * block_width + x + 1];
                    }
                    else
                    {
                        block_items[fill[y * block_width + x]++] = b;
                    }
                }
            }
        }

        if (pass == 0)
        {
            for (x = 0; x < num_cells; ++x)
            {
                block_first[x + 1] += block_first[x];
            }

            block_items = malloc(SDL_max(block_first[num_cells], 1) * sizeof(*block_items));
            fill = malloc(num_cells * sizeof(*fill));
            memcpy(fill, block_first, num_cells * sizeof(*fill));
        }
    }

    free(fill);
    free(block_seen);
    block_seen = calloc(SDL_max(map.num_blocks, 1), sizeof(*block_seen));
    block_query = 0;
}

// Every solid thing that is not a monster, as the level starts. Monsters
// move about: they are steered round, not cut out of the floor. A thing
// outside the blockmap never stops anything (PIT_CheckThing only meets
// things in it).
static void CollectBlocks(void)
{
    const thinker_t *thinker;
    int capacity = 64;

    free(map.blocks);
    free(block_types);
    map.blocks = malloc(capacity * sizeof(*map.blocks));
    block_types = malloc(capacity * sizeof(*block_types));
    map.num_blocks = 0;
    map.num_barrels = 0;

    for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
    {
        const mobj_t *mo = (const mobj_t *) thinker;
        k_block_t *block;

        if (thinker->function.acp1 != (actionf_p1) P_MobjThinker
         || !(mo->flags & MF_SOLID) || (mo->flags & (MF_COUNTKILL | MF_NOBLOCKMAP))
         || mo->type == MT_SKULL || mo == players[consoleplayer].mo)
        {
            continue;
        }

        if (map.num_blocks == capacity)
        {
            capacity *= 2;
            map.blocks = realloc(map.blocks, capacity * sizeof(*map.blocks));
            block_types = realloc(block_types, capacity * sizeof(*block_types));
        }

        block = &map.blocks[map.num_blocks];
        block->at.x = FixedToMap(mo->x);
        block->at.y = FixedToMap(mo->y);
        block->radius = FixedToMap(mo->radius);
        block->barrel = mo->type == MT_BARREL;
        block_types[map.num_blocks++] = mo->type;
        map.num_barrels += block->barrel;
    }

    free(block_standing);
    block_standing = malloc(SDL_max(map.num_blocks, 1));
    memset(block_standing, 1, SDL_max(map.num_blocks, 1));
    blocks_judged = 0;

    BuildBlockGrid();
}

// The blocks each cell lies under: those whose squares hold its centroid.
// Cells are cut along every square they overlap, so each lies wholly under
// a square or wholly outside it.
static void FindCellBlocks(void)
{
    int c, b, pass;

    free(cell_block_first);
    free(cell_block_list);
    cell_block_first = calloc(map.num_cells + 1, sizeof(*cell_block_first));
    cell_block_list = NULL;

    for (pass = 0; pass < 2; ++pass)
    {
        for (c = 0; c < map.num_cells; ++c)
        {
            k_cell_t *cell = &map.cells[c];
            int count = 0;

            cell->block = -1;

            for (b = 0; b < map.num_blocks; ++b)
            {
                const k_block_t *block = &map.blocks[b];

                if (fabs(cell->centroid.x - block->at.x) >= block->radius
                 || fabs(cell->centroid.y - block->at.y) >= block->radius)
                {
                    continue;
                }

                if (cell->block < 0)
                {
                    cell->block = b;
                }

                if (pass == 1)
                {
                    cell_block_list[cell_block_first[c] + count] = b;
                }

                ++count;
            }

            if (pass == 0)
            {
                cell_block_first[c + 1] = count;
            }
        }

        if (pass == 0)
        {
            for (c = 0; c < map.num_cells; ++c)
            {
                cell_block_first[c + 1] += cell_block_first[c];
            }

            cell_block_list = malloc(SDL_max(cell_block_first[map.num_cells], 1)
                                     * sizeof(*cell_block_list));
        }
    }
}

// Whether a block's thing is still in the blockmap, solid, near where it
// stood. Looked up by place, not by pointer: the memory of a thing that is
// gone is used again.
static boolean ThingStands(int index)
{
    const k_block_t *block = &map.blocks[index];
    fixed_t x = MapToFixed(block->at.x);
    fixed_t y = MapToFixed(block->at.y);
    fixed_t drift = MapToFixed(BLOCK_DRIFT);
    int x0 = SDL_max(0, (x - drift - bmaporgx) >> MAPBLOCKSHIFT);
    int x1 = SDL_min(bmapwidth - 1, (x + drift - bmaporgx) >> MAPBLOCKSHIFT);
    int y0 = SDL_max(0, (y - drift - bmaporgy) >> MAPBLOCKSHIFT);
    int y1 = SDL_min(bmapheight - 1, (y + drift - bmaporgy) >> MAPBLOCKSHIFT);
    int bx, by;

    for (by = y0; by <= y1; ++by)
    {
        for (bx = x0; bx <= x1; ++bx)
        {
            const mobj_t *mo;

            for (mo = blocklinks[by * bmapwidth + bx]; mo != NULL; mo = mo->bnext)
            {
                if (mo->type == block_types[index] && (mo->flags & MF_SOLID)
                 && abs(mo->x - x) <= drift && abs(mo->y - y) <= drift)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

//
// Floor cells
//

static void AddCell(int subsector, const k_vec2_t *poly, int n)
{
    k_cell_t *cell;
    int i;

    if (map.num_cells == cells_capacity)
    {
        cells_capacity = SDL_max(cells_capacity * 2, 1024);
        map.cells = realloc(map.cells, cells_capacity * sizeof(*map.cells));
    }

    if (map.num_points + n > points_capacity)
    {
        points_capacity = SDL_max(points_capacity * 2, map.num_points + n + 1024);
        map.points = realloc(map.points, points_capacity * sizeof(*map.points));
    }

    cell = &map.cells[map.num_cells++];
    cell->subsector = subsector;
    cell->first_point = map.num_points;
    cell->num_points = n;
    cell->area = PolygonArea(poly, n, &cell->centroid);
    cell->min = cell->max = poly[0];
    cell->centroid_mismatch = 0;

    for (i = 1; i < n; ++i)
    {
        ExtendBounds(&cell->min, &cell->max, &poly[i]);
    }

    memcpy(&map.points[map.num_points], poly, n * sizeof(*poly));
    map.num_points += n;
}

// For subsectors without floor: a centroid among the segs, for display.
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

// Cuts the pieces a block's square overlaps into up to four round the
// square and one under it. Edge by edge round the square, what lies beyond
// the edge is kept as a piece and the rest goes on to the next edge; what
// is left after all four lies under the square. A piece the square misses,
// or only touches, stays whole. Returns the new piece count.
static int CutAroundBlock(piece_t **pieces, int *capacity, int count,
                          const k_block_t *block)
{
    k_vec2_t min, max, corners[4];
    int total = count;
    int i, e;

    // Counter-clockwise: the square is on the left of its edges.
    BlockSquare(block, &min, &max);
    corners[0] = min;
    corners[1].x = max.x;
    corners[1].y = min.y;
    corners[2] = max;
    corners[3].x = min.x;
    corners[3].y = max.y;

    // Each piece gives four more at most.
    if (5 * count > *capacity)
    {
        *capacity = 5 * count;
        *pieces = realloc(*pieces, *capacity * sizeof(**pieces));
    }

    for (i = 0; i < count; ++i)
    {
        piece_t *piece = &(*pieces)[i];

        for (e = 0; e < 4; ++e)
        {
            k_vec2_t dir = { corners[(e + 1) % 4].x - corners[e].x,
                             corners[(e + 1) % 4].y - corners[e].y };
            piece_t beyond, within;

            beyond.points = malloc((piece->count + 2) * sizeof(*beyond.points));
            within.points = malloc((piece->count + 2) * sizeof(*within.points));
            beyond.count = CleanPolygon(beyond.points,
                ClipPolygon(piece->points, piece->count, beyond.points, corners[e], dir,
                            true));
            within.count = CleanPolygon(within.points,
                ClipPolygon(piece->points, piece->count, within.points, corners[e], dir,
                            false));

            // Wholly beyond this edge: clear of the square.
            if (HasNoArea(within.points, within.count))
            {
                free(beyond.points);
                free(within.points);
                break;
            }

            if (HasNoArea(beyond.points, beyond.count))
            {
                free(beyond.points);
                free(within.points);
                continue;
            }

            free(piece->points);
            *piece = within;
            (*pieces)[total++] = beyond;
        }
    }

    return total;
}

// Adds a piece of floor as cells, cut round the squares of the blocks on
// it.
static void AddFloor(int subsector, const k_vec2_t *poly, int n)
{
    piece_t *pieces;
    k_vec2_t min, max, block_min, block_max;
    int count = 1, capacity = 4;
    int i;

    pieces = malloc(capacity * sizeof(*pieces));
    pieces[0].points = malloc(n * sizeof(*poly));
    memcpy(pieces[0].points, poly, n * sizeof(*poly));
    pieces[0].count = n;

    min = max = poly[0];

    for (i = 1; i < n; ++i)
    {
        ExtendBounds(&min, &max, &poly[i]);
    }

    for (i = 0; i < map.num_blocks; ++i)
    {
        BlockSquare(&map.blocks[i], &block_min, &block_max);

        if (block_max.x > min.x && block_min.x < max.x
         && block_max.y > min.y && block_min.y < max.y)
        {
            count = CutAroundBlock(&pieces, &capacity, count, &map.blocks[i]);
        }
    }

    for (i = 0; i < count; ++i)
    {
        AddCell(subsector, pieces[i].points, pieces[i].count);
        free(pieces[i].points);
    }

    free(pieces);
}

// Splits every piece by the line, keeping both sides where they have area.
// Returns the new piece count.
static int SplitPieces(piece_t **pieces, int *capacity, int count,
                       k_vec2_t origin, k_vec2_t dir)
{
    int total = count;
    int i, side;

    // Each piece splits in two at most.
    if (2 * count > *capacity)
    {
        *capacity = 2 * count;
        *pieces = realloc(*pieces, *capacity * sizeof(**pieces));
    }

    for (i = 0; i < count; ++i)
    {
        piece_t *piece = &(*pieces)[i];
        piece_t halves[2];

        for (side = 0; side < 2; ++side)
        {
            halves[side].points = malloc((piece->count + 2)
                                         * sizeof(*halves[side].points));
            halves[side].count = CleanPolygon(halves[side].points,
                ClipPolygon(piece->points, piece->count,
                            halves[side].points, origin, dir, side == 0));
        }

        // The line misses the piece, or only grazes it.
        if (HasNoArea(halves[0].points, halves[0].count)
         || HasNoArea(halves[1].points, halves[1].count))
        {
            free(halves[0].points);
            free(halves[1].points);
            continue;
        }

        free(piece->points);
        *piece = halves[0];
        (*pieces)[total++] = halves[1];
    }

    return total;
}

static void BuildLeaf(int index, const k_vec2_t *poly, int n)
{
    const subsector_t *source;
    k_subsector_t *ss;
    piece_t *pieces;
    k_vec2_t min, max;
    int count, capacity, i;
    double cx = 0.0, cy = 0.0;

    if (index < 0 || index >= numsubsectors || reached[index])
    {
        return;
    }

    reached[index] = 1;
    source = &subsectors[index];
    ss = &map.subsectors[index];
    ss->sector = SectorIndex(source->sector);
    ss->first_cell = map.num_cells;

    // Leaves with no usable floor need no cells: some nodebuilders split
    // along the same line twice, leaving a leaf with segs but no area, and
    // vertex rounding leaves hairline slivers along lines.
    if (HasNoArea(poly, n))
    {
        ss->no_area = 1;
        PlaceAmongSegs(ss, source);
        return;
    }

    capacity = 4;
    pieces = malloc(capacity * sizeof(*pieces));
    pieces[0].points = malloc(n * sizeof(*poly));
    memcpy(pieces[0].points, poly, n * sizeof(*poly));
    pieces[0].count = n;
    count = 1;

    min = max = poly[0];

    for (i = 1; i < n; ++i)
    {
        ExtendBounds(&min, &max, &poly[i]);
    }

    // Split the region by every linedef passing through it, so that each
    // piece lies within one sector or wholly in the void. The region's own
    // segs are not enough: void is never visible, so nodebuilders give it
    // to whichever leaf is convenient, walls or not. Most lines miss the
    // region or only run along its edge.
    for (i = 0; i < map.num_lines; ++i)
    {
        const k_line_t *line = &map.lines[i];
        k_vec2_t dir = { line->v2.x - line->v1.x, line->v2.y - line->v1.y };

        if (SDL_max(line->v1.x, line->v2.x) < min.x
         || SDL_min(line->v1.x, line->v2.x) > max.x
         || SDL_max(line->v1.y, line->v2.y) < min.y
         || SDL_min(line->v1.y, line->v2.y) > max.y
         || !SegmentCrossesPolygon(line->v1, line->v2, poly, n))
        {
            continue;
        }

        count = SplitPieces(&pieces, &capacity, count, line->v1, dir);
    }

    // Keep the pieces that are floor. A piece the surrounding lines put in
    // another sector (sector tricks, buggy nodes) still belongs to this
    // subsector as far as the engine is concerned, so keep it, but note it.
    for (i = 0; i < count; ++i)
    {
        k_vec2_t center;
        int sector;

        PolygonArea(pieces[i].points, pieces[i].count, &center);
        sector = SectorAtPoint(center);

        if (sector >= 0)
        {
            AddFloor(index, pieces[i].points, pieces[i].count);
            ss->foreign_floor |= sector != ss->sector;
        }

        free(pieces[i].points);
    }

    free(pieces);

    ss->num_cells = map.num_cells - ss->first_cell;

    // No floor at all: the whole region is solid space, e.g. inside a
    // pillar. Nodebuilders leave these behind thin walls.
    if (ss->num_cells == 0)
    {
        ss->behind_wall = 1;
        PlaceAmongSegs(ss, source);
        return;
    }

    ss->area = 0.0;
    ss->min = map.cells[ss->first_cell].min;
    ss->max = map.cells[ss->first_cell].max;

    for (i = ss->first_cell; i < map.num_cells; ++i)
    {
        ss->area += map.cells[i].area;
        cx += map.cells[i].centroid.x * map.cells[i].area;
        cy += map.cells[i].centroid.y * map.cells[i].area;
        ExtendBounds(&ss->min, &ss->max, &map.cells[i].min);
        ExtendBounds(&ss->min, &ss->max, &map.cells[i].max);
    }

    ss->centroid.x = cx / ss->area;
    ss->centroid.y = cy / ss->area;
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

static void BuildCells(void)
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
    map.num_cells = 0;
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

static void CheckCells(void)
{
    int i;

    map.split_subsectors = 0;
    map.no_area_subsectors = 0;
    map.behind_wall_subsectors = 0;
    map.foreign_floor_subsectors = 0;
    map.unreached_subsectors = 0;
    map.centroid_mismatches = 0;

    for (i = 0; i < map.num_subsectors; ++i)
    {
        const k_subsector_t *ss = &map.subsectors[i];

        if (!reached[i])
        {
            if (map.unreached_subsectors++ < MAX_REPORTS)
            {
                K_Log(K_LOG_MAP, K_LOG_WARN, "subsector %d is not reachable "
                      "through the BSP tree", i);
            }
        }
        else if (ss->no_area)
        {
            ++map.no_area_subsectors;
        }
        else if (ss->behind_wall)
        {
            ++map.behind_wall_subsectors;
        }
        else
        {
            map.split_subsectors += ss->num_cells > 1;
            map.foreign_floor_subsectors += ss->foreign_floor;
        }
    }

    for (i = 0; i < map.num_cells; ++i)
    {
        k_cell_t *cell = &map.cells[i];
        int found = (int) (R_PointInSubsector(MapToFixed(cell->centroid.x),
                                              MapToFixed(cell->centroid.y))
                           - subsectors);

        if (found != cell->subsector)
        {
            cell->centroid_mismatch = 1;

            if (map.centroid_mismatches++ < MAX_REPORTS)
            {
                K_Log(K_LOG_MAP, K_LOG_WARN, "cell of subsector %d: centroid "
                      "(%.0f,%.0f) lies in subsector %d", cell->subsector,
                      cell->centroid.x, cell->centroid.y, found);
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
    CollectBlocks();
    BuildCells();
    FindCellBlocks();
    CheckCells();

    map.loaded = 1;
    ++map.generation;
    map.build_ms = (SDL_GetPerformanceCounter() - start) * 1000.0
                 / SDL_GetPerformanceFrequency();

    problems = map.unreached_subsectors + map.centroid_mismatches;

    K_Log(K_LOG_MAP, problems > 0 ? K_LOG_WARN : K_LOG_INFO,
          "%s: %d lines, %d sectors, %d subsectors (%d without area, %d behind "
          "walls, %d split, %d with other-sector floor) in %d cells, %d solid "
          "things (%d barrels) - %d unreached, %d centroid mismatches, %.1f ms",
          map.name, map.num_lines, map.num_sectors, map.num_subsectors,
          map.no_area_subsectors, map.behind_wall_subsectors,
          map.split_subsectors, map.foreign_floor_subsectors, map.num_cells,
          map.num_blocks, map.num_barrels, map.unreached_subsectors,
          map.centroid_mismatches, map.build_ms);
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

int K_Map_CellAt(double x, double y)
{
    const k_subsector_t *ss;
    int index, i;

    if (!map.loaded || map.num_subsectors == 0)
    {
        return -1;
    }

    // The BSP walk always lands in some subsector, even from the void.
    index = (int) (R_PointInSubsector(MapToFixed(x), MapToFixed(y)) - subsectors);

    if (index < 0 || index >= map.num_subsectors)
    {
        return -1;
    }

    ss = &map.subsectors[index];

    for (i = ss->first_cell; i < ss->first_cell + ss->num_cells; ++i)
    {
        if (PointInCell(&map.cells[i], x, y, INSIDE_TOLERANCE))
        {
            return i;
        }
    }

    // The engine can place a point in a hairline sliver without floor, or
    // just across a partition line from a cell it is within tolerance of.
    return K_Map_NearestCell(x, y, INSIDE_TOLERANCE);
}

// 0 inside the cell, otherwise the distance to its nearest edge.
static double CellDistance(const k_cell_t *cell, k_vec2_t point)
{
    const k_vec2_t *poly = &map.points[cell->first_point];
    boolean inside = true;
    double best = 1e30;
    int i;

    for (i = 0; i < cell->num_points; ++i)
    {
        k_vec2_t a = poly[i];
        k_vec2_t b = poly[(i + 1) % cell->num_points];
        k_vec2_t ab = { b.x - a.x, b.y - a.y };
        double t = ((point.x - a.x) * ab.x + (point.y - a.y) * ab.y)
                 / SDL_max(ab.x * ab.x + ab.y * ab.y, 1e-12);
        double dx, dy;

        if (LineSide(a, ab, point) < 0.0)
        {
            inside = false;
        }

        t = SDL_max(0.0, SDL_min(1.0, t));
        dx = point.x - (a.x + ab.x * t);
        dy = point.y - (a.y + ab.y * t);
        best = SDL_min(best, sqrt(dx * dx + dy * dy));
    }

    return inside ? 0.0 : best;
}

int K_Map_NearestCell(double x, double y, double max_distance)
{
    k_vec2_t point = { x, y };
    double best_distance = max_distance;
    int best = -1;
    int i;

    for (i = 0; i < map.num_cells; ++i)
    {
        const k_cell_t *cell = &map.cells[i];
        double distance;

        if (x < cell->min.x - best_distance || x > cell->max.x + best_distance
         || y < cell->min.y - best_distance || y > cell->max.y + best_distance)
        {
            continue;
        }

        distance = CellDistance(cell, point);

        if (distance <= best_distance)
        {
            best_distance = distance;
            best = i;

            if (distance == 0.0)
            {
                break;
            }
        }
    }

    return best;
}

static double CellFloor(int index)
{
    int sector = map.subsectors[map.cells[index].subsector].sector;

    return sector >= 0 ? FixedToMap(sectors[sector].floorheight) : -1e30;
}

// Whether a cell overlaps the square of half-width r around (x, y): no
// edge of the convex cell has every corner of the square outside it.
static boolean CellOverlapsBox(const k_cell_t *cell, double x, double y, double r)
{
    const k_vec2_t *poly = &map.points[cell->first_point];
    int i, c;

    if (x + r < cell->min.x || x - r > cell->max.x
     || y + r < cell->min.y || y - r > cell->max.y)
    {
        return false;
    }

    for (i = 0; i < cell->num_points; ++i)
    {
        k_vec2_t a = poly[i];
        k_vec2_t b = poly[(i + 1) % cell->num_points];
        k_vec2_t dir = { b.x - a.x, b.y - a.y };

        for (c = 0; c < 4; ++c)
        {
            k_vec2_t corner = { x + ((c & 1) ? r : -r), y + ((c & 2) ? r : -r) };

            if (LineSide(a, dir, corner) >= 0.0)
            {
                break;
            }
        }

        if (c == 4)
        {
            return false;
        }
    }

    return true;
}

int K_Map_StandingCell(double x, double y, double floor)
{
    return K_Map_StandingCellOn(x, y, floor, CellFloor);
}

int K_Map_StandingCellOn(double x, double y, double floor, k_cellfloor_t cell_floor)
{
    k_vec2_t point = { x, y };
    int cell = K_Map_CellAt(x, y);
    double best_distance = 1e30;
    int best = -1;
    int i;

    // Up to a step above the floor under the centre (the edge of a stair
    // under the box, say), the centre's cell is as good to plan from: the
    // player can step up again from there. A tiny cell under the box would
    // be worse, e.g. a ledge in front of a door, too shallow for the player.
    if (cell >= 0 && floor - cell_floor(cell) <= MAX_STEP_UP)
    {
        return cell;
    }

    for (i = 0; i < map.num_cells; ++i)
    {
        double distance;

        if (fabs(cell_floor(i) - floor) >= 0.5
         || !CellOverlapsBox(&map.cells[i], x, y, PLAYER_RADIUS)
         || K_Map_CellBlock(i) >= 0)
        {
            continue;
        }

        distance = CellDistance(&map.cells[i], point);

        if (distance < best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }

    return best >= 0 ? best : cell;
}

int K_Map_SubsectorAt(double x, double y)
{
    int cell = K_Map_CellAt(x, y);

    return cell >= 0 ? map.cells[cell].subsector : -1;
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

int K_Map_BlockStands(int block)
{
    int b;

    if (!map.loaded || block < 0 || block >= map.num_blocks)
    {
        return 0;
    }

    if (blocks_judged != leveltime + 1)
    {
        for (b = 0; b < map.num_blocks; ++b)
        {
            block_standing[b] = ThingStands(b);
        }

        blocks_judged = leveltime + 1;
    }

    return block_standing[block];
}

int K_Map_CellBlock(int cell)
{
    int i;

    if (!map.loaded || cell < 0 || cell >= map.num_cells)
    {
        return -1;
    }

    for (i = cell_block_first[cell]; i < cell_block_first[cell + 1]; ++i)
    {
        if (K_Map_BlockStands(cell_block_list[i]))
        {
            return cell_block_list[i];
        }
    }

    return -1;
}

int K_Map_BlocksNear(k_vec2_t min, k_vec2_t max, int *found, int max_found)
{
    int count = 0;
    int x, y, x0, y0, x1, y1, k;

    if (!map.loaded || map.num_blocks == 0)
    {
        return 0;
    }

    if (++block_query == 0)
    {
        memset(block_seen, 0, map.num_blocks * sizeof(*block_seen));
        block_query = 1;
    }

    BlockRange(min, max, &x0, &y0, &x1, &y1);

    for (y = y0; y <= y1; ++y)
    {
        for (x = x0; x <= x1; ++x)
        {
            int grid = y * block_width + x;

            for (k = block_first[grid]; k < block_first[grid + 1]; ++k)
            {
                int b = block_items[k];
                k_vec2_t block_min, block_max;

                // A block over several grid cells comes up more than once.
                if (block_seen[b] == block_query)
                {
                    continue;
                }

                block_seen[b] = block_query;
                BlockSquare(&map.blocks[b], &block_min, &block_max);

                if (block_max.x < min.x || block_min.x > max.x
                 || block_max.y < min.y || block_min.y > max.y
                 || count >= max_found || !K_Map_BlockStands(b))
                {
                    continue;
                }

                found[count++] = b;
            }
        }
    }

    return count;
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
    pose->momx = FixedToMap(mo->momx);
    pose->momy = FixedToMap(mo->momy);
    pose->onground = mo->z <= mo->floorz;
    pose->frozen = mo->reactiontime;
    pose->floor = FixedToMap(mo->floorz);
    pose->cell = K_Map_StandingCell(pose->x, pose->y, pose->floor);
    return 1;
}
