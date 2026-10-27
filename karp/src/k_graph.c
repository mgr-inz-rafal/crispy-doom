//
// DESCRIPTION:
//	Karp navigation graph. Two floor cells are neighbours where their
//	edges lie on the same line, run in opposite directions (all cells are
//	counter-clockwise) and overlap; each overlap becomes a portal. Edges
//	are bucketed in a uniform grid so large maps stay fast.
//
//	Teleporters are not portals: a portal along a teleport line is crossed
//	from the line's front side into the teleporter's destination, wherever
//	that is, not into the cell beyond (K_Graph_Teleport).
//
//	Sanity check: every cell edge should be covered by portals and
//	one-sided walls. Whatever is left borders nothing - a hole in the
//	navigation mesh.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomdata.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "p_local.h"
#include "r_state.h"

#include "k_graph.h"
#include "k_log.h"
#include "k_specials.h"

#define K_PI 3.14159265358979323846

// Vanilla nodebuilders round split vertices to whole map units, so the
// edges two neighbours share do not line up exactly: they can be off by
// up to this much in the middle of the shared stretch, and twice that at
// its ends (two slightly different angles drift apart). A thin wall would
// separate them by its full thickness along the whole stretch.
#define MATCH_TOLERANCE 1.0

// The shortest shared stretch that makes a portal.
#define MIN_OVERLAP 1.0

// The most linedefs one portal is found to run along (then split per line).
#define MAX_PORTAL_LINES 16

// A gap in a cell's border counts as a hole in the navigation mesh only if
// it is at least this long and this far from every wall. Shorter gaps are
// rounding at corners; gaps along a wall are hairline slivers of void.
#define MIN_HOLE 4.0
#define NEAR_WALL 2.0

#define GRID_CELL 128.0

// Player movement limits (p_map.c: P_TryMove, PIT_CheckLine).
#define MAX_STEP_UP (24 * FRACUNIT)
#define PLAYER_HEIGHT (56 * FRACUNIT)
#define PLAYER_RADIUS 16.0

// Movers (p_doors.c, p_plats.c), in map units per tic: doors move at 2,
// blazing ones at 8; lifts at 4, blazing ones at 8. A lift waits 3 seconds
// at the bottom.
#define DOOR_SPEED (2 * FRACUNIT)
#define LIFT_SPEED (4 * FRACUNIT)
#define BLAZING_SPEED (8 * FRACUNIT)
#define LIFT_WAIT (3 * TICRATE)

// Each time a lift turned back while the player rode it (something in its
// way that the map does not show: a monster at its edge, a barrel), riding
// it counts TURN_WAIT tics more, up to MAX_TURNS times: still well under
// what a blocked crossing adds. A route another way wins once it is worth
// it; where there is none, the lift stays the way, however often it turns.
#define TURN_WAIT (5 * TICRATE)
#define MAX_TURNS 100

// Tics to stop in front of a line, face it and press use.
#define ACTION_OVERHEAD 35.0

// A teleported player can neither move nor turn for this many tics
// (EV_Teleport).
#define TELEPORT_FREEZE 18.0

// A crossing is a way through only if some point on it is this far from
// anything that stops the player: its radius (16), plus a unit. A gap
// exactly as wide as the player (32) needs the player lined up to the
// fraction of a unit (Doom II MAP02 has one): too hard, so 34 is the least.
#define CLEARANCE 17.0
#define CLEARANCE_SAMPLES 9
#define MAX_NEAR_LINES 256

// A gap inside a cell, between its portals, can be narrower than the
// player too: a closed block beside a wall, mid-cell (Doom II MAP21's
// yellow bars leave 32 units). Chords across cells, at most two clearances
// long, from the corners and the ends of lines on their borders, are
// judged like crossings: sampled every CHORD_STEP, and CHORD_REACH past
// their ends too, since the player's box can hang over open floor beyond
// (else every thin cell beside a wall would look like a gap).
#define MAX_CHORD (2.0 * CLEARANCE + 1.0)
#define CHORD_REACH (CLEARANCE - 1.0)
#define CHORD_STEP 2.0
#define MAX_CHORD_POINTS 64

// Where a route crosses a portal: this far from anything that stops the
// player where the portal allows (more than its radius, so it does not
// clip wall corners), and otherwise as near the middle as it can be.
#define WAYPOINT_CLEARANCE 24.0
#define WAYPOINT_STEP 4.0
#define MAX_WAYPOINT_SAMPLES 64

// Per kind of problem, only this many are logged individually.
#define MAX_REPORTS 5

typedef struct
{
    int cell;
    k_vec2_t a, b;
} edge_t;

// A covered stretch of an edge, as distances from its start.
typedef struct
{
    int edge;
    double t0, t1;
} cover_t;

static k_graph_t graph;
static int portals_capacity;

static edge_t *edges;
static int num_edges;

static cover_t *covers;
static int num_covers;
static int covers_capacity;

// Grid items are edges (0 .. num_edges-1), then linedefs.
static k_vec2_t grid_origin;
static int grid_width, grid_height;
static int *cell_first;     // per grid cell, into cell_items; one extra at
                            // the end
static int *cell_items;

static const char *crossing_names[K_NUM_CROSSINGS] =
{
    "open", "wall", "blocking line", "step too high", "no headroom",
    "locked door", "too narrow", "door", "lift", "lift ride", "teleporter",
};

// Per sector: opened by a manual door line (it is the line's back
// sector), or moved by a lift line (it has the line's tag).
static unsigned char *door_sectors;
static unsigned char *door_walled;      // a door with a one-sided line: set in a wall
static unsigned char *lift_sectors;
static unsigned char *lift_turns;       // per sector, K_Graph_LiftTurnedBack
static k_vec2_t *door_min, *door_max;   // per door sector, its bounding box

// Per line: the teleporter it belongs to, -1 if none.
static int *line_teleports;
static int teleports_capacity;

// Chords across cells where a gap inside one may be narrower than the
// player (K_Graph_Pinched), grouped by cell.
typedef struct
{
    k_vec2_t a, b;
    int judged;             // leveltime + 1 when last judged, 0 never
    boolean narrow;
} chord_t;

static chord_t *chords;
static int num_chords;
static int chords_capacity;
static int *chord_first;    // per cell, into chords; one extra at the end

//
// Vector helpers
//

static k_vec2_t Sub(k_vec2_t a, k_vec2_t b)
{
    k_vec2_t result = { a.x - b.x, a.y - b.y };

    return result;
}

static double Dot(k_vec2_t a, k_vec2_t b)
{
    return a.x * b.x + a.y * b.y;
}

static double Length(k_vec2_t v)
{
    return sqrt(v.x * v.x + v.y * v.y);
}

static k_vec2_t PointAlong(k_vec2_t a, k_vec2_t b, double t)
{
    double length = Length(Sub(b, a));
    k_vec2_t result;

    result.x = a.x + (b.x - a.x) * t / length;
    result.y = a.y + (b.y - a.y) * t / length;
    return result;
}

// Distance of point p from the segment a-b.
static double SegmentDistance(k_vec2_t a, k_vec2_t b, k_vec2_t p)
{
    k_vec2_t ab = Sub(b, a);
    double t = Dot(Sub(p, a), ab) / SDL_max(Dot(ab, ab), 1e-12);
    k_vec2_t closest;

    t = SDL_max(0.0, SDL_min(1.0, t));
    closest.x = a.x + ab.x * t;
    closest.y = a.y + ab.y * t;
    return Length(Sub(p, closest));
}

// If p-q runs along a-b for at least MIN_OVERLAP, returns true with the
// shared stretch as distances from a. Only that stretch has to lie on both
// lines: vanilla nodebuilders round split vertices to whole map units, so
// a seg (and a cell edge built from it) can be slightly off its linedef's
// angle, which adds up over a long line.
//
// The stretch must lie along the segment p-q itself, not just its line,
// and be MIN_OVERLAP long measured along both: two edges meeting at a
// corner, or nearly at right angles, share no stretch.
static boolean Overlap(k_vec2_t a, k_vec2_t b, k_vec2_t p, k_vec2_t q,
                       double *t0, double *t1)
{
    double length = Length(Sub(b, a));
    double other_length = Length(Sub(q, p));
    k_vec2_t dir;
    double tp, tq;

    if (length < MIN_OVERLAP || other_length < MIN_OVERLAP)
    {
        return false;
    }

    dir.x = (b.x - a.x) / length;
    dir.y = (b.y - a.y) / length;
    tp = Dot(dir, Sub(p, a));
    tq = Dot(dir, Sub(q, a));
    *t0 = SDL_max(0.0, SDL_min(tp, tq));
    *t1 = SDL_min(length, SDL_max(tp, tq));

    return (*t1 - *t0) * fabs(tq - tp) / other_length >= MIN_OVERLAP
        && SegmentDistance(p, q, PointAlong(a, b, *t0)) <= 2.0 * MATCH_TOLERANCE
        && SegmentDistance(p, q, PointAlong(a, b, *t1)) <= 2.0 * MATCH_TOLERANCE
        && SegmentDistance(p, q, PointAlong(a, b, (*t0 + *t1) * 0.5))
               <= MATCH_TOLERANCE;
}

//
// Grid
//

static void ItemBounds(const k_map_t *map, int item, k_vec2_t *min,
                       k_vec2_t *max)
{
    k_vec2_t a, b;

    if (item < num_edges)
    {
        a = edges[item].a;
        b = edges[item].b;
    }
    else
    {
        a = map->lines[item - num_edges].v1;
        b = map->lines[item - num_edges].v2;
    }

    min->x = SDL_min(a.x, b.x) - 2.0 * MATCH_TOLERANCE;
    min->y = SDL_min(a.y, b.y) - 2.0 * MATCH_TOLERANCE;
    max->x = SDL_max(a.x, b.x) + 2.0 * MATCH_TOLERANCE;
    max->y = SDL_max(a.y, b.y) + 2.0 * MATCH_TOLERANCE;
}

static void CellRange(k_vec2_t min, k_vec2_t max, int *x0, int *y0,
                      int *x1, int *y1)
{
    *x0 = SDL_max(0, (int) floor((min.x - grid_origin.x) / GRID_CELL));
    *y0 = SDL_max(0, (int) floor((min.y - grid_origin.y) / GRID_CELL));
    *x1 = SDL_min(grid_width - 1, (int) floor((max.x - grid_origin.x) / GRID_CELL));
    *y1 = SDL_min(grid_height - 1, (int) floor((max.y - grid_origin.y) / GRID_CELL));
}

static void BuildGrid(const k_map_t *map)
{
    int num_items = num_edges + map->num_lines;
    int num_cells, item, x, y, x0, y0, x1, y1;
    k_vec2_t min, max;
    int *fill;

    // Cells extend a little past the lines (the map's bounding box
    // padding), so leave a margin.
    grid_origin.x = map->min.x - 2 * GRID_CELL;
    grid_origin.y = map->min.y - 2 * GRID_CELL;
    grid_width = (int) ((map->max.x - grid_origin.x) / GRID_CELL) + 3;
    grid_height = (int) ((map->max.y - grid_origin.y) / GRID_CELL) + 3;
    num_cells = grid_width * grid_height;

    // Count the items per grid cell, then fill.
    free(cell_first);
    cell_first = calloc(num_cells + 1, sizeof(*cell_first));

    for (item = 0; item < num_items; ++item)
    {
        ItemBounds(map, item, &min, &max);
        CellRange(min, max, &x0, &y0, &x1, &y1);

        for (y = y0; y <= y1; ++y)
        {
            for (x = x0; x <= x1; ++x)
            {
                ++cell_first[y * grid_width + x + 1];
            }
        }
    }

    for (x = 0; x < num_cells; ++x)
    {
        cell_first[x + 1] += cell_first[x];
    }

    free(cell_items);
    cell_items = malloc(SDL_max(cell_first[num_cells], 1) * sizeof(*cell_items));
    fill = malloc(num_cells * sizeof(*fill));
    memcpy(fill, cell_first, num_cells * sizeof(*fill));

    for (item = 0; item < num_items; ++item)
    {
        ItemBounds(map, item, &min, &max);
        CellRange(min, max, &x0, &y0, &x1, &y1);

        for (y = y0; y <= y1; ++y)
        {
            for (x = x0; x <= x1; ++x)
            {
                cell_items[fill[y * grid_width + x]++] = item;
            }
        }
    }

    free(fill);
}

//
// Building
//

static void CollectEdges(const k_map_t *map)
{
    int i, j;

    free(edges);
    edges = malloc(SDL_max(map->num_points, 1) * sizeof(*edges));
    num_edges = 0;

    for (i = 0; i < map->num_cells; ++i)
    {
        const k_cell_t *cell = &map->cells[i];
        const k_vec2_t *poly = &map->points[cell->first_point];

        for (j = 0; j < cell->num_points; ++j)
        {
            edges[num_edges].cell = i;
            edges[num_edges].a = poly[j];
            edges[num_edges].b = poly[(j + 1) % cell->num_points];
            ++num_edges;
        }
    }
}

static void AddCover(int edge, double t0, double t1)
{
    if (num_covers == covers_capacity)
    {
        covers_capacity = SDL_max(covers_capacity * 2, 1024);
        covers = realloc(covers, covers_capacity * sizeof(*covers));
    }

    covers[num_covers].edge = edge;
    covers[num_covers].t0 = t0;
    covers[num_covers].t1 = t1;
    ++num_covers;
}

// Room for one more portal; returns its index.
static int NewPortal(void)
{
    if (graph.num_portals == portals_capacity)
    {
        portals_capacity = SDL_max(portals_capacity * 2, 1024);
        graph.portals = realloc(graph.portals,
                                portals_capacity * sizeof(*graph.portals));
    }

    return graph.num_portals++;
}

// The stretch t0-t1 of edge e is shared with edge f.
static void AddPortal(int e, int f, double t0, double t1)
{
    k_portal_t *portal;
    double f0, f1;

    portal = &graph.portals[NewPortal()];
    portal->cell[0] = edges[e].cell;
    portal->cell[1] = edges[f].cell;
    portal->a = PointAlong(edges[e].a, edges[e].b, t0);
    portal->b = PointAlong(edges[e].a, edges[e].b, t1);
    portal->line = -1;
    portal->teleport_from = -1;

    AddCover(e, t0, t1);

    // The same stretch, measured along f.
    if (Overlap(edges[f].a, edges[f].b, portal->a, portal->b, &f0, &f1))
    {
        AddCover(f, f0, f1);
    }
}

static void MatchEdges(const k_map_t *map)
{
    int num_items = num_edges + map->num_lines;
    int *stamp = malloc(SDL_max(num_items, 1) * sizeof(*stamp));
    int i, k, x, y, x0, y0, x1, y1;
    k_vec2_t min, max;
    double t0, t1;

    // stamp[item] == i: item already looked at for edge i.
    for (k = 0; k < num_items; ++k)
    {
        stamp[k] = -1;
    }

    for (i = 0; i < num_edges; ++i)
    {
        const edge_t *e = &edges[i];

        ItemBounds(map, i, &min, &max);
        CellRange(min, max, &x0, &y0, &x1, &y1);

        for (y = y0; y <= y1; ++y)
        {
            for (x = x0; x <= x1; ++x)
            {
                int cell = y * grid_width + x;

                for (k = cell_first[cell]; k < cell_first[cell + 1]; ++k)
                {
                    int item = cell_items[k];

                    if (stamp[item] == i)
                    {
                        continue;
                    }

                    stamp[item] = i;

                    if (item < num_edges)
                    {
                        const edge_t *f = &edges[item];

                        // Each pair once; opposite directions only.
                        if (item > i && f->cell != e->cell
                         && Dot(Sub(e->b, e->a), Sub(f->b, f->a)) < 0.0
                         && Overlap(e->a, e->b, f->a, f->b, &t0, &t1))
                        {
                            AddPortal(i, item, t0, t1);
                        }
                    }
                    else
                    {
                        const k_line_t *line = &map->lines[item - num_edges];

                        // Only walls bound the floor.
                        if (line->back_sector < 0
                         && Overlap(e->a, e->b, line->v1, line->v2, &t0, &t1))
                        {
                            AddCover(i, t0, t1);
                        }
                    }
                }
            }
        }
    }

    free(stamp);
}

// The linedef a portal runs along, if any. A two-sided line is expected;
// a one-sided one would be a map error and makes the portal a wall.
static int FindPortalLine(const k_map_t *map, const k_portal_t *portal)
{
    k_vec2_t mid = { (portal->a.x + portal->b.x) * 0.5,
                     (portal->a.y + portal->b.y) * 0.5 };
    int x0, y0, x1, y1;
    int found = -1;
    int cell, k;
    double t0, t1;

    CellRange(mid, mid, &x0, &y0, &x1, &y1);
    cell = y0 * grid_width + x0;

    for (k = cell_first[cell]; k < cell_first[cell + 1]; ++k)
    {
        int item = cell_items[k];
        const k_line_t *line;

        if (item < num_edges)
        {
            continue;
        }

        line = &map->lines[item - num_edges];

        if (!Overlap(line->v1, line->v2, portal->a, portal->b, &t0, &t1))
        {
            continue;
        }

        if (line->back_sector >= 0)
        {
            return item - num_edges;
        }

        found = item - num_edges;
    }

    return found;
}

// A stretch of a portal that runs along a two-sided line, as distances
// from the portal's start.
typedef struct
{
    double t0, t1;
    int line;
} stretch_t;

static int CompareStretches(const void *a, const void *b)
{
    const stretch_t *sa = a, *sb = b;

    return sa->t0 < sb->t0 ? -1 : sa->t0 > sb->t0 ? 1 : 0;
}

// The two-sided lines a portal runs along, up to max.
static int PortalStretches(const k_map_t *map, const k_portal_t *portal,
                           stretch_t *found, int max)
{
    k_vec2_t min = { SDL_min(portal->a.x, portal->b.x) - 2.0 * MATCH_TOLERANCE,
                     SDL_min(portal->a.y, portal->b.y) - 2.0 * MATCH_TOLERANCE };
    k_vec2_t max_ = { SDL_max(portal->a.x, portal->b.x) + 2.0 * MATCH_TOLERANCE,
                      SDL_max(portal->a.y, portal->b.y) + 2.0 * MATCH_TOLERANCE };
    int x, y, x0, y0, x1, y1, k, i;
    int count = 0;
    double t0, t1;

    CellRange(min, max_, &x0, &y0, &x1, &y1);

    for (y = y0; y <= y1; ++y)
    {
        for (x = x0; x <= x1; ++x)
        {
            int cell = y * grid_width + x;

            for (k = cell_first[cell]; k < cell_first[cell + 1]; ++k)
            {
                int line = cell_items[k] - num_edges;

                if (line < 0 || map->lines[line].back_sector < 0)
                {
                    continue;
                }

                // A line spanning several grid cells comes up more than once.
                for (i = 0; i < count && found[i].line != line; ++i)
                {
                }

                if (i < count || count >= max
                 || !Overlap(portal->a, portal->b, map->lines[line].v1,
                             map->lines[line].v2, &t0, &t1))
                {
                    continue;
                }

                found[count].t0 = t0;
                found[count].t1 = t1;
                found[count].line = line;
                ++count;
            }
        }
    }

    return count;
}

// Gives each portal its line. A portal can run along several collinear
// lines (a diagonal door between two short jambs, Doom II MAP12): it is
// split where each line starts, so every portal runs along one line and
// is judged by it - its special, whether it blocks the player.
static void SplitPortals(const k_map_t *map)
{
    int count = graph.num_portals;
    int i, k;

    for (i = 0; i < count; ++i)
    {
        stretch_t stretches[MAX_PORTAL_LINES];
        k_portal_t whole = graph.portals[i];
        double length = Length(Sub(whole.b, whole.a));
        int n = PortalStretches(map, &whole, stretches, MAX_PORTAL_LINES);
        double start = 0.0;
        int line, piece = i;

        // A miniseg, or (a map error) along a one-sided line only.
        if (n == 0)
        {
            graph.portals[i].line = FindPortalLine(map, &whole);
            continue;
        }

        qsort(stretches, n, sizeof(*stretches), CompareStretches);
        line = stretches[0].line;

        // A piece runs from where its line starts to where the next one
        // does; slivers shorter than MIN_OVERLAP stay with the piece before.
        for (k = 1; k <= n; ++k)
        {
            double end = k < n ? stretches[k].t0 : length;

            if (k < n && (end - start < MIN_OVERLAP || length - end < MIN_OVERLAP))
            {
                continue;
            }

            graph.portals[piece] = whole;
            graph.portals[piece].a = PointAlong(whole.a, whole.b, start);
            graph.portals[piece].b = PointAlong(whole.a, whole.b, end);
            graph.portals[piece].line = line;

            if (k < n)
            {
                piece = NewPortal();
                start = end;
                line = stretches[k].line;
            }
        }
    }
}

static boolean NearWall(const k_map_t *map, k_vec2_t point)
{
    k_vec2_t min = { point.x - NEAR_WALL, point.y - NEAR_WALL };
    k_vec2_t max = { point.x + NEAR_WALL, point.y + NEAR_WALL };
    int x, y, x0, y0, x1, y1, k;

    CellRange(min, max, &x0, &y0, &x1, &y1);

    for (y = y0; y <= y1; ++y)
    {
        for (x = x0; x <= x1; ++x)
        {
            int cell = y * grid_width + x;

            for (k = cell_first[cell]; k < cell_first[cell + 1]; ++k)
            {
                int item = cell_items[k];
                const k_line_t *line;

                if (item < num_edges)
                {
                    continue;
                }

                line = &map->lines[item - num_edges];

                if (line->back_sector < 0
                 && SegmentDistance(line->v1, line->v2, point) <= NEAR_WALL)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

static void BuildLinks(const k_map_t *map)
{
    int count = SDL_max(map->num_cells, 1);
    int *fill;
    int i;

    free(graph.first_link);
    free(graph.link_count);
    free(graph.links);
    graph.first_link = calloc(count, sizeof(int));
    graph.link_count = calloc(count, sizeof(int));
    graph.num_links = 2 * graph.num_portals;
    graph.links = malloc(SDL_max(graph.num_links, 1) * sizeof(*graph.links));

    for (i = 0; i < graph.num_portals; ++i)
    {
        ++graph.link_count[graph.portals[i].cell[0]];
        ++graph.link_count[graph.portals[i].cell[1]];
    }

    for (i = 1; i < map->num_cells; ++i)
    {
        graph.first_link[i] = graph.first_link[i - 1] + graph.link_count[i - 1];
    }

    fill = malloc(count * sizeof(*fill));
    memcpy(fill, graph.first_link, count * sizeof(*fill));

    for (i = 0; i < graph.num_portals; ++i)
    {
        int c0 = graph.portals[i].cell[0];
        int c1 = graph.portals[i].cell[1];

        graph.links[fill[c0]].portal = i;
        graph.links[fill[c0]++].neighbor = c1;
        graph.links[fill[c1]].portal = i;
        graph.links[fill[c1]++].neighbor = c0;
    }

    free(fill);
}

//
// Sanity checks
//

static int CompareCovers(const void *a, const void *b)
{
    const cover_t *ca = a, *cb = b;

    if (ca->edge != cb->edge)
    {
        return ca->edge < cb->edge ? -1 : 1;
    }

    return ca->t0 < cb->t0 ? -1 : ca->t0 > cb->t0 ? 1 : 0;
}

// The stretch t0-t1 of edge e borders neither a neighbour nor a wall.
static void CheckGap(const k_map_t *map, int e, double t0, double t1)
{
    k_vec2_t at;

    if (t1 - t0 < MIN_HOLE)
    {
        return;
    }

    at = PointAlong(edges[e].a, edges[e].b, (t0 + t1) * 0.5);

    if (NearWall(map, at))
    {
        return;
    }

    graph.hole_length += t1 - t0;

    if (graph.holes++ < MAX_REPORTS)
    {
        K_Log(K_LOG_MAP, K_LOG_WARN, "a cell of subsector %d borders nothing "
              "for %.0f units around (%.0f,%.0f)",
              map->cells[edges[e].cell].subsector, t1 - t0, at.x, at.y);
    }
}

static void CheckHoles(const k_map_t *map)
{
    int i, c = 0;

    graph.holes = 0;
    graph.hole_length = 0.0;

    qsort(covers, num_covers, sizeof(*covers), CompareCovers);

    for (i = 0; i < num_edges; ++i)
    {
        double length = Length(Sub(edges[i].b, edges[i].a));
        double reach = 0.0;         // covered up to here

        for (; c < num_covers && covers[c].edge == i; ++c)
        {
            if (covers[c].t0 > reach)
            {
                CheckGap(map, i, reach, covers[c].t0);
            }

            reach = SDL_max(reach, covers[c].t1);
        }

        if (length > reach)
        {
            CheckGap(map, i, reach, length);
        }
    }
}

static int FindRoot(int *parent, int i)
{
    while (parent[i] != i)
    {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }

    return i;
}

static void CheckConnectivity(const k_map_t *map)
{
    int count = SDL_max(map->num_cells, 1);
    int *parent = malloc(count * sizeof(*parent));
    int *size = calloc(count, sizeof(*size));
    int i;

    graph.isolated_cells = 0;
    graph.components = 0;
    graph.largest_component = 0;

    for (i = 0; i < map->num_cells; ++i)
    {
        parent[i] = i;
    }

    for (i = 0; i < graph.num_portals; ++i)
    {
        int a = FindRoot(parent, graph.portals[i].cell[0]);
        int b = FindRoot(parent, graph.portals[i].cell[1]);

        parent[a] = b;
    }

    for (i = 0; i < map->num_cells; ++i)
    {
        int root = FindRoot(parent, i);

        graph.isolated_cells += graph.link_count[i] == 0;

        if (size[root]++ == 0)
        {
            ++graph.components;
        }

        graph.largest_component = SDL_max(graph.largest_component, size[root]);
    }

    free(parent);
    free(size);
}

//
// Doors and lifts
//

// Doors opened by using their own line (EV_VerticalDoor); the door is the
// line's back sector.
static boolean IsManualDoor(int special)
{
    switch (special)
    {
        case 1: case 26: case 27: case 28:      // open, wait, close
        case 31: case 32: case 33: case 34:     // open and stay
        case 117: case 118:                     // blazing
            return true;

        default:
            return false;
    }
}

static boolean IsBlazingDoor(int special)
{
    return special == 117 || special == 118;
}

// Lifts lowered by using a line (S1, SR, blazing S1 and SR); the lift is
// the sectors with the line's tag.
static boolean IsLiftSwitch(int special)
{
    return special == 21 || special == 62 || special == 122 || special == 123;
}

static boolean IsBlazingLift(int special)
{
    return special == 120 || special == 121 || special == 122 || special == 123;
}

// Every line that moves a lift: switches, walk-over lines, perpetual lifts.
static boolean IsLiftLine(int special)
{
    return IsLiftSwitch(special) || special == 10 || special == 88
        || special == 120 || special == 121 || special == 53 || special == 87;
}

// From the specials at level load: the kind of a sector does not change
// when a one-shot line loses its special.
static void FindMoverSectors(const k_map_t *map)
{
    int count = SDL_max(numsectors, 1);
    int i, s;

    free(door_sectors);
    free(door_walled);
    free(lift_sectors);
    free(lift_turns);
    door_sectors = calloc(count, 1);
    door_walled = calloc(count, 1);
    lift_sectors = calloc(count, 1);
    lift_turns = calloc(count, 1);

    for (i = 0; i < numlines; ++i)
    {
        const line_t *line = &lines[i];

        if (IsManualDoor(line->special) && map->lines[i].back_sector >= 0)
        {
            door_sectors[map->lines[i].back_sector] = 1;
        }

        if (IsLiftLine(line->special) && line->tag != 0)
        {
            for (s = 0; s < numsectors; ++s)
            {
                if (sectors[s].tag == line->tag)
                {
                    lift_sectors[s] = 1;
                }
            }
        }
    }

    free(door_min);
    free(door_max);
    door_min = malloc(count * sizeof(*door_min));
    door_max = malloc(count * sizeof(*door_max));

    for (s = 0; s < count; ++s)
    {
        door_min[s].x = door_min[s].y = HUGE_VAL;
        door_max[s].x = door_max[s].y = -HUGE_VAL;
    }

    for (i = 0; i < numlines; ++i)
    {
        const k_line_t *line = &map->lines[i];
        int side;

        for (side = 0; side < 2; ++side)
        {
            s = side ? line->back_sector : line->front_sector;

            if (s >= 0 && door_sectors[s])
            {
                door_min[s].x = SDL_min(door_min[s].x, SDL_min(line->v1.x, line->v2.x));
                door_min[s].y = SDL_min(door_min[s].y, SDL_min(line->v1.y, line->v2.y));
                door_max[s].x = SDL_max(door_max[s].x, SDL_max(line->v1.x, line->v2.x));
                door_max[s].y = SDL_max(door_max[s].y, SDL_max(line->v1.y, line->v2.y));

                if (line->back_sector < 0)
                {
                    door_walled[s] = 1;
                }
            }
        }
    }
}

//
// Teleporters
//

// Walk-over lines that teleport the player (W1, WR); 125 and 126 teleport
// only monsters.
static boolean IsTeleportLine(int special)
{
    return special == 39 || special == 97;
}

// Where crossing a teleport line with this tag sends the player, as
// EV_Teleport finds it: the first teleport destination, in thinker order,
// in the first sector with the tag.
static const mobj_t *FindDestination(int tag)
{
    const thinker_t *thinker;
    int s;

    for (s = 0; s < numsectors; ++s)
    {
        if (sectors[s].tag != tag)
        {
            continue;
        }

        for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
        {
            const mobj_t *mo;

            if (thinker->function.acp1 != (actionf_p1) P_MobjThinker)
            {
                continue;
            }

            mo = (const mobj_t *) thinker;

            if (mo->type == MT_TELEPORTMAN && mo->subsector->sector == &sectors[s])
            {
                return mo;
            }
        }
    }

    return NULL;
}

static int AddTeleporter(int tag)
{
    const mobj_t *to = FindDestination(tag);
    k_teleport_t *teleport;

    if (graph.num_teleports == teleports_capacity)
    {
        teleports_capacity = SDL_max(teleports_capacity * 2, 16);
        graph.teleports = realloc(graph.teleports,
                                  teleports_capacity * sizeof(*graph.teleports));
    }

    teleport = &graph.teleports[graph.num_teleports];
    teleport->tag = tag;
    teleport->to.x = teleport->to.y = 0.0;
    teleport->angle = 0.0;
    teleport->cell = -1;

    if (to != NULL)
    {
        teleport->to.x = (double) to->x / FRACUNIT;
        teleport->to.y = (double) to->y / FRACUNIT;
        teleport->angle = to->angle * (2.0 * K_PI / 4294967296.0);
        teleport->cell = K_Map_CellAt(teleport->to.x, teleport->to.y);
    }

    if (teleport->cell < 0)
    {
        K_Log(K_LOG_MAP, K_LOG_WARN, "teleport lines with tag %d lead nowhere: %s",
              tag, to == NULL ? "no destination in a sector with the tag"
                              : "the destination is on no floor");
    }

    return graph.num_teleports++;
}

// From the specials at level load: a one-shot line loses its special when
// used, but stays part of its teleporter. The destination does not move.
static void FindTeleporters(const k_map_t *map)
{
    int i, t;

    free(line_teleports);
    line_teleports = malloc(SDL_max(numlines, 1) * sizeof(*line_teleports));
    graph.num_teleports = 0;

    for (i = 0; i < numlines; ++i)
    {
        const k_line_t *line = &map->lines[i];
        k_teleport_t *teleport;

        line_teleports[i] = -1;

        if (!IsTeleportLine(lines[i].special) || line->back_sector < 0)
        {
            continue;
        }

        for (t = 0; t < graph.num_teleports && graph.teleports[t].tag != lines[i].tag; ++t)
        {
        }

        if (t == graph.num_teleports)
        {
            t = AddTeleporter(lines[i].tag);
            graph.teleports[t].min = graph.teleports[t].max = line->v1;
        }

        // Padded: the portals along the lines are rounded off them a little.
        teleport = &graph.teleports[t];
        teleport->min.x = SDL_min(teleport->min.x, SDL_min(line->v1.x, line->v2.x) - 2.0 * MATCH_TOLERANCE);
        teleport->min.y = SDL_min(teleport->min.y, SDL_min(line->v1.y, line->v2.y) - 2.0 * MATCH_TOLERANCE);
        teleport->max.x = SDL_max(teleport->max.x, SDL_max(line->v1.x, line->v2.x) + 2.0 * MATCH_TOLERANCE);
        teleport->max.y = SDL_max(teleport->max.y, SDL_max(line->v1.y, line->v2.y) + 2.0 * MATCH_TOLERANCE);
        line_teleports[i] = t;
    }

    // The front side of a line is on the right of v1 -> v2.
    for (i = 0; i < graph.num_portals; ++i)
    {
        k_portal_t *portal = &graph.portals[i];
        const k_line_t *line;
        k_vec2_t c;

        if (portal->line < 0 || line_teleports[portal->line] < 0)
        {
            continue;
        }

        line = &map->lines[portal->line];
        c = map->cells[portal->cell[0]].centroid;
        portal->teleport_from =
            (line->v2.x - line->v1.x) * (c.y - line->v1.y)
          - (line->v2.y - line->v1.y) * (c.x - line->v1.x) < 0.0
            ? portal->cell[0] : portal->cell[1];
    }
}

//
// Pinches
//

static boolean OnBorder(const k_vec2_t *poly, int n, k_vec2_t p)
{
    int i;

    for (i = 0; i < n; ++i)
    {
        if (SegmentDistance(poly[i], poly[(i + 1) % n], p) < 1.0)
        {
            return true;
        }
    }

    return false;
}

static boolean SamePoint(k_vec2_t a, k_vec2_t b)
{
    return fabs(a.x - b.x) < 0.5 && fabs(a.y - b.y) < 0.5;
}

// A chord of the cell being built (from chord_first[cell] on), if it goes
// across it: not too long, not along an edge, not there already.
static void TryChord(int cell, const k_vec2_t *poly, int n, k_vec2_t a, k_vec2_t b)
{
    double length = Length(Sub(b, a));
    k_vec2_t middle = { (a.x + b.x) * 0.5, (a.y + b.y) * 0.5 };
    int i;

    if (length < 1.0 || length > MAX_CHORD || OnBorder(poly, n, middle))
    {
        return;
    }

    for (i = chord_first[cell]; i < num_chords; ++i)
    {
        if ((SamePoint(chords[i].a, a) && SamePoint(chords[i].b, b))
         || (SamePoint(chords[i].a, b) && SamePoint(chords[i].b, a)))
        {
            return;
        }
    }

    if (num_chords == chords_capacity)
    {
        chords_capacity = SDL_max(chords_capacity * 2, 256);
        chords = realloc(chords, chords_capacity * sizeof(*chords));
    }

    chords[num_chords].a = a;
    chords[num_chords].b = b;
    chords[num_chords].judged = 0;
    chords[num_chords].narrow = false;
    ++num_chords;
}

// The narrowest way across a convex cell between two obstacles on its
// border starts at the end of one of them: a corner, or the end of a line
// partway along an edge. From each such point, a chord to the nearest
// point of every other edge, and to each other such point.
static void FindChords(const k_map_t *map)
{
    int c;

    num_chords = 0;
    free(chord_first);
    chord_first = malloc((map->num_cells + 1) * sizeof(*chord_first));

    for (c = 0; c < map->num_cells; ++c)
    {
        const k_cell_t *cell = &map->cells[c];
        const k_vec2_t *poly = &map->points[cell->first_point];
        int n = cell->num_points;
        k_vec2_t points[MAX_CHORD_POINTS];
        k_vec2_t min = { cell->min.x - 1.0, cell->min.y - 1.0 };
        k_vec2_t max = { cell->max.x + 1.0, cell->max.y + 1.0 };
        int num_points = 0;
        int x, y, x0, y0, x1, y1, k, i, j, end;

        chord_first[c] = num_chords;

        for (i = 0; i < n && num_points < MAX_CHORD_POINTS; ++i)
        {
            points[num_points++] = poly[i];
        }

        CellRange(min, max, &x0, &y0, &x1, &y1);

        for (y = y0; y <= y1; ++y)
        {
            for (x = x0; x <= x1; ++x)
            {
                int grid = y * grid_width + x;

                for (k = cell_first[grid]; k < cell_first[grid + 1]; ++k)
                {
                    int line = cell_items[k] - num_edges;
                    k_vec2_t ends[2];

                    if (line < 0)
                    {
                        continue;
                    }

                    ends[0] = map->lines[line].v1;
                    ends[1] = map->lines[line].v2;

                    for (end = 0; end < 2; ++end)
                    {
                        for (j = 0; j < num_points && !SamePoint(points[j], ends[end]); ++j)
                        {
                        }

                        if (j == num_points && num_points < MAX_CHORD_POINTS
                         && OnBorder(poly, n, ends[end]))
                        {
                            points[num_points++] = ends[end];
                        }
                    }
                }
            }
        }

        for (i = 0; i < num_points; ++i)
        {
            for (j = 0; j < n; ++j)
            {
                k_vec2_t a = poly[j], ab = Sub(poly[(j + 1) % n], poly[j]);
                double t = SDL_max(0.0, SDL_min(1.0, Dot(Sub(points[i], a), ab)
                                                     / SDL_max(Dot(ab, ab), 1e-12)));
                k_vec2_t nearest = { a.x + ab.x * t, a.y + ab.y * t };

                TryChord(c, poly, n, points[i], nearest);
            }

            for (j = i + 1; j < num_points; ++j)
            {
                TryChord(c, poly, n, points[i], points[j]);
            }
        }
    }

    chord_first[map->num_cells] = num_chords;
}

void K_Graph_Build(void)
{
    const k_map_t *map = K_Map_Get();
    Uint64 start = SDL_GetPerformanceCounter();

    graph.num_portals = 0;
    num_covers = 0;

    CollectEdges(map);
    BuildGrid(map);
    MatchEdges(map);
    SplitPortals(map);
    BuildLinks(map);
    FindMoverSectors(map);
    FindTeleporters(map);
    FindChords(map);
    CheckHoles(map);
    CheckConnectivity(map);

    graph.generation = map->generation;
    graph.build_ms = (SDL_GetPerformanceCounter() - start) * 1000.0
                   / SDL_GetPerformanceFrequency();

    K_Log(K_LOG_MAP, graph.holes > 0 ? K_LOG_WARN : K_LOG_INFO,
          "%s graph: %d portals, %d teleporters, %d chords, %d holes (%.0f units), "
          "%d isolated, %d components (largest %d of %d cells), %.1f ms",
          map->name, graph.num_portals, graph.num_teleports, num_chords, graph.holes,
          graph.hole_length, graph.isolated_cells, graph.components,
          graph.largest_component, map->num_cells, graph.build_ms);
}

const k_graph_t *K_Graph_Get(void)
{
    return &graph;
}

static const plat_t *PlatOf(const sector_t *sector)
{
    const thinker_t *thinker = sector->specialdata;

    if (thinker != NULL && thinker->function.acp1 == (actionf_p1) T_PlatRaise)
    {
        return (const plat_t *) thinker;
    }

    return NULL;
}

static const vldoor_t *DoorOf(const sector_t *sector)
{
    const thinker_t *thinker = sector->specialdata;

    if (thinker != NULL && thinker->function.acp1 == (actionf_p1) T_VerticalDoor)
    {
        return (const vldoor_t *) thinker;
    }

    return NULL;
}

// How high a door opens (EV_VerticalDoor): the lowest surrounding ceiling,
// less 4. Live, since the surrounding ceilings can move.
static fixed_t DoorTop(sector_t *sector)
{
    return P_FindLowestCeilingSurrounding(sector) - 4 * FRACUNIT;
}

// The ceiling of a sector, taking a door already opening (a switch opened
// it) as open, to where it is going.
static fixed_t OpeningCeiling(sector_t *sector)
{
    const vldoor_t *door = DoorOf(sector);
    fixed_t top = sector->ceilingheight;

    if (door != NULL && door->direction == 1)
    {
        top = SDL_max(top, door->topheight);
    }

    return top;
}

// The ceiling of a sector the player stands in: standing in a door means
// it is open.
static fixed_t OpenCeiling(sector_t *sector)
{
    fixed_t top = OpeningCeiling(sector);

    if (door_sectors[sector - sectors])
    {
        top = SDL_max(top, DoorTop(sector));
    }

    return top;
}

// Where the player is, for judging doors: in up to two sectors (-1 for
// none), its centre somewhere on the segment from a to b. A door nearer
// than reach all along it is open (FaceCeiling).
typedef struct
{
    int sector[2];
    k_vec2_t a, b;
    double reach;
} standing_t;

// The larger of the x and y distances from a point to a box, 0 inside.
static double BoxGap(k_vec2_t at, k_vec2_t min, k_vec2_t max)
{
    double dx = SDL_max(0.0, SDL_max(min.x - at.x, at.x - max.x));
    double dy = SDL_max(0.0, SDL_max(min.y - at.y, at.y - max.y));

    return SDL_max(dx, dy);
}

// Whether the door sector's bounding box is within reach of the player
// wherever its centre is on the segment: the gap is convex along it, so
// both ends tell.
static boolean NearDoor(const standing_t *standing, int sector)
{
    return BoxGap(standing->a, door_min[sector], door_max[sector]) < standing->reach
        && BoxGap(standing->b, door_min[sector], door_max[sector]) < standing->reach;
}

// Whether using the line opens the door behind it: a manual door line,
// from its front side, and the key it needs, if any.
static boolean OpensDoor(const line_t *line)
{
    int key = K_Specials_LockKey(line->special);

    return IsManualDoor(line->special) && (key < 0 || K_Specials_HasKey(key));
}

// The ceiling of the sector on one side of a line, as a player beside the
// line meets it. A door counts as open where the player stands in it, or is
// near it all along: at a crossing, near enough that the closed door alone
// would leave no way through, so the player gets there only through the
// open door (Doom II MAP16 and MAP03: one-way doors leave strips 8 and 16
// deep behind them; MAP16's blue door, locked, at the back of an alcove
// only 16 deep). A door set in a wall (it has a one-sided line) also counts
// as open seen across a face that opens it (OpensDoor): there is no way
// past it but through it, so whatever its closed faces narrow is on that
// way (MAP01: a light sector in the 24-deep room before the exit door;
// MAP24: the 17-deep alcove before an exit door). Else a closed door's
// faces would narrow every crossing near a doorway. Any other door is a
// block, so a way past it leads through it, as a door crossing, and gets it
// opened: MAP26's yellow-key pillar, 16 square in a corridor 64 wide, every
// face a door line, leaves 24 units on either side, and counted open, the
// bot squeezed past it and stuck; MAP21's yellow bars, doors on all faces,
// leave a 24-unit gap between two; MAP16's red-key pillar in the mouth of
// the corridor to the exit, opened from two of its four faces, gaps exactly
// as wide as the player.
static fixed_t FaceCeiling(const line_t *line, int side, const standing_t *standing)
{
    sector_t *sector = side ? line->backsector : line->frontsector;
    int index = (int) (sector - sectors);

    if (index == standing->sector[0] || index == standing->sector[1]
     || (side == 1 && door_walled[index] && OpensDoor(line))
     || (door_sectors[index] && NearDoor(standing, index)))
    {
        return OpenCeiling(sector);
    }

    return OpeningCeiling(sector);
}

// Where a lift goes down to (EV_DoPlat): the lowest surrounding floor.
static fixed_t LiftBottom(sector_t *sector)
{
    return SDL_min(P_FindLowestFloorSurrounding(sector), sector->floorheight);
}

static const floormove_t *FloorMoverOf(const sector_t *sector)
{
    const thinker_t *thinker = sector->specialdata;

    if (thinker != NULL && thinker->function.acp1 == (actionf_p1) T_MoveFloor)
    {
        return (const floormove_t *) thinker;
    }

    return NULL;
}

// How high a floor the player stands on will rise, and how fast: an active
// lift or plat goes (back) up to its top, a floor moving up to where it is
// going; anything else stays where it is. Unless it crushes, it stops where
// the player's head meets the ceiling, and a lift turns back down there
// (T_PlatRaise): Doom II MAP24's red key platform tops out 48 units under
// its ceiling, so with the player on it, it rises to 120 of its 128.
static fixed_t RisingTop(const sector_t *sector, fixed_t *speed)
{
    const plat_t *plat = PlatOf(sector);
    const floormove_t *floor = FloorMoverOf(sector);
    fixed_t top = sector->floorheight;
    boolean crush = false;

    *speed = LIFT_SPEED;

    if (plat != NULL)
    {
        *speed = plat->speed;
        top = plat->high;
        crush = plat->crush;
    }
    else if (floor != NULL && floor->direction == 1)
    {
        *speed = floor->speed;
        top = floor->floordestheight;
        crush = floor->crush;
    }

    if (!crush)
    {
        top = SDL_min(top, sector->ceilingheight - PLAYER_HEIGHT);
    }

    return SDL_max(top, sector->floorheight);
}

// The extra wait for riding a lift that has turned back on the player.
static double TurnWait(int sector)
{
    return (double) lift_turns[sector] * TURN_WAIT;
}

// Whether a lift will come down without being worked: a perpetual one
// (specials 53, 87; stopped ones have no thinker running), or one on its
// way down.
static boolean LiftComing(const sector_t *sector)
{
    const plat_t *plat = PlatOf(sector);

    return plat != NULL && (plat->type == perpetualRaise || plat->status == down);
}

// Tics until a lift that is coming is at the bottom (EV_DoPlat,
// T_PlatRaise): going up, it waits at the top before coming down.
static double LiftArrival(const sector_t *sector)
{
    const plat_t *plat = PlatOf(sector);
    double down = (double) (sector->floorheight - plat->low) / plat->speed;

    switch (plat->status)
    {
        case up:
            return (double) (plat->high - sector->floorheight) / plat->speed
                 + plat->wait + (double) (plat->high - plat->low) / plat->speed;

        case waiting:
            return plat->count + down;

        default:
            return down;
    }
}

static boolean Headroom(fixed_t floor_a, fixed_t ceiling_a,
                        fixed_t floor_b, fixed_t ceiling_b)
{
    return SDL_min(ceiling_a, ceiling_b) - SDL_max(floor_a, floor_b) >= PLAYER_HEIGHT
        && ceiling_b - floor_b >= PLAYER_HEIGHT;
}

//
// Clearance
//

// The floor and ceiling of the sector a cell is in. Standing in a door
// means it is open.
static void StandingHeights(int cell, fixed_t *floor, fixed_t *ceiling)
{
    const k_map_t *map = K_Map_Get();
    int index = map->subsectors[map->cells[cell].subsector].sector;
    sector_t *sector = &sectors[index];

    *floor = sector->floorheight;
    *ceiling = OpenCeiling(sector);
}

// The heights a player crossing a portal stands at when its box gets to a
// line near it. The engine stands the player on the highest floor under
// its box, under the lowest ceiling. Before the portal that is the cell it
// comes from. A line wholly past the portal is reached with the box over
// the cell beyond too, so a step up there is already climbed: stairs whose
// steps are shallower than the player's radius would otherwise look too
// narrow, the next riser counting as a wall (Doom II MAP03). Not past a
// step too high (a lift to lower, a ride up): the player never stands
// there as things are.
typedef struct
{
    const k_portal_t *portal;
    k_vec2_t normal;            // unit, pointing past the portal
    fixed_t floor[2];           // [0] before the portal, [1] past it
    fixed_t ceiling[2];
    standing_t standing;        // on the portal, in the cells on either side
    boolean crossing;           // a real portal: a line along it is the
                                // crossing, not beside it
} stance_t;

static void Stance(int portal_index, int from, stance_t *stance)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &graph.portals[portal_index];
    int to = portal->cell[0] == from ? portal->cell[1] : portal->cell[0];
    k_vec2_t dir = Sub(portal->b, portal->a);
    double length = SDL_max(Length(dir), 1e-6);

    stance->portal = portal;
    stance->normal.x = -dir.y / length;
    stance->normal.y = dir.x / length;

    if (Dot(stance->normal, Sub(map->cells[to].centroid, portal->a)) < 0.0)
    {
        stance->normal.x = -stance->normal.x;
        stance->normal.y = -stance->normal.y;
    }

    StandingHeights(from, &stance->floor[0], &stance->ceiling[0]);
    StandingHeights(to, &stance->floor[1], &stance->ceiling[1]);
    stance->standing.sector[0] = map->subsectors[map->cells[from].subsector].sector;
    stance->standing.sector[1] = map->subsectors[map->cells[to].subsector].sector;
    stance->standing.a = portal->a;
    stance->standing.b = portal->b;
    stance->standing.reach = CLEARANCE;
    stance->crossing = true;

    if (stance->floor[1] - stance->floor[0] <= MAX_STEP_UP)
    {
        stance->floor[1] = SDL_max(stance->floor[0], stance->floor[1]);
        stance->ceiling[1] = SDL_min(stance->ceiling[0], stance->ceiling[1]);
    }
    else
    {
        stance->floor[1] = stance->floor[0];
        stance->ceiling[1] = stance->ceiling[0];
        stance->standing.sector[1] = stance->standing.sector[0];
    }
}

// 1 if a line lies wholly past the portal: no end before its line, and
// not along it.
static int StanceSide(const stance_t *stance, const k_line_t *line)
{
    double d1 = Dot(stance->normal, Sub(line->v1, stance->portal->a));
    double d2 = Dot(stance->normal, Sub(line->v2, stance->portal->a));

    return d1 > -1.0 && d2 > -1.0 && SDL_max(d1, d2) >= 1.0;
}

// Whether a line stops a player standing on the given floor under the
// given ceiling from overlapping it (PIT_CheckLine): a wall, an impassable
// line, or a two-sided line with a side too high to step onto or too low
// to fit under. A drop is no obstacle: the player can hang over it.
// Doors as FaceCeiling takes them.
static boolean Obstacle(int index, fixed_t floor, fixed_t ceiling,
                        const standing_t *standing)
{
    const line_t *line = &lines[index];
    int side;

    if (line->backsector == NULL || (line->flags & ML_BLOCKING))
    {
        return true;
    }

    for (side = 0; side < 2; ++side)
    {
        sector_t *sector = side ? line->backsector : line->frontsector;
        fixed_t top = FaceCeiling(line, side, standing);

        if (sector->floorheight - floor > MAX_STEP_UP
         || !Headroom(floor, ceiling, sector->floorheight, top))
        {
            return true;
        }
    }

    return false;
}

// The line the portal itself runs along is the crossing, not beside it:
// it lies on the portal's line and overlaps it.
static boolean AlongPortal(const k_line_t *line, const k_portal_t *portal)
{
    k_vec2_t dir = Sub(portal->b, portal->a);
    double length = Length(dir);
    double t1, t2;

    dir.x /= length;
    dir.y /= length;

    if (fabs(dir.x * (line->v1.y - portal->a.y) - dir.y * (line->v1.x - portal->a.x)) > 1.0
     || fabs(dir.x * (line->v2.y - portal->a.y) - dir.y * (line->v2.x - portal->a.x)) > 1.0)
    {
        return false;
    }

    t1 = Dot(dir, Sub(line->v1, portal->a));
    t2 = Dot(dir, Sub(line->v2, portal->a));
    return SDL_min(length, SDL_max(t1, t2)) - SDL_max(0.0, SDL_min(t1, t2)) > 1.0;
}

// The obstacle lines within reach of a portal, for a player crossing it.
static int NearObstacles(const stance_t *stance, double reach, int *found, int max)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = stance->portal;
    k_vec2_t min = { SDL_min(portal->a.x, portal->b.x) - reach - 1.0,
                     SDL_min(portal->a.y, portal->b.y) - reach - 1.0 };
    k_vec2_t max_ = { SDL_max(portal->a.x, portal->b.x) + reach + 1.0,
                      SDL_max(portal->a.y, portal->b.y) + reach + 1.0 };
    int x, y, x0, y0, x1, y1, k, i;
    int count = 0;
    CellRange(min, max_, &x0, &y0, &x1, &y1);

    for (y = y0; y <= y1; ++y)
    {
        for (x = x0; x <= x1; ++x)
        {
            int cell = y * grid_width + x;

            for (k = cell_first[cell]; k < cell_first[cell + 1]; ++k)
            {
                int line = cell_items[k] - num_edges;
                int side;

                if (line < 0)
                {
                    continue;
                }

                // A line spanning several grid cells comes up more than once.
                for (i = 0; i < count && found[i] != line; ++i)
                {
                }

                if (i < count || count >= max
                 || (stance->crossing && AlongPortal(&map->lines[line], portal)))
                {
                    continue;
                }

                side = StanceSide(stance, &map->lines[line]);

                if (!Obstacle(line, stance->floor[side], stance->ceiling[side],
                              &stance->standing))
                {
                    continue;
                }

                found[count++] = line;
            }
        }
    }

    return count;
}

// Whether no point on the portal is CLEARANCE away from every obstacle.
static boolean Narrow(const stance_t *stance)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = stance->portal;
    int found[MAX_NEAR_LINES];
    int count = NearObstacles(stance, CLEARANCE, found, MAX_NEAR_LINES);
    int s, i;

    for (s = 0; s < CLEARANCE_SAMPLES; ++s)
    {
        k_vec2_t at = PointAlong(portal->a, portal->b,
                                 Length(Sub(portal->b, portal->a))
                                 * (s + 0.5) / CLEARANCE_SAMPLES);

        for (i = 0; i < count; ++i)
        {
            if (SegmentDistance(map->lines[found[i]].v1, map->lines[found[i]].v2, at)
                < CLEARANCE)
            {
                break;
            }
        }

        if (i == count)
        {
            return false;
        }
    }

    return true;
}

// Mirrors P_TryMove/PIT_CheckLine with live heights, then asks whether a
// door or lift on the way can be worked from this side. wait gets the
// tics the action is expected to take.
static k_crossing_t JudgeHeights(int portal_index, int from, double *wait)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &graph.portals[portal_index];
    int to = portal->cell[0] == from ? portal->cell[1] : portal->cell[0];
    int from_sector = map->subsectors[map->cells[from].subsector].sector;
    int to_sector = map->subsectors[map->cells[to].subsector].sector;
    const line_t *line = NULL;
    sector_t *a, *b;
    fixed_t floor_a, ceiling_a, floor_b, ceiling_b;

    *wait = 0.0;

    if (from_sector < 0 || to_sector < 0)
    {
        return K_CROSS_WALL;
    }

    if (portal->line >= 0)
    {
        // The engine's line, for its live special: one-shot lines lose it.
        line = &lines[portal->line];

        if (map->lines[portal->line].back_sector < 0)
        {
            return K_CROSS_WALL;
        }

        // A portal runs along one line (SplitPortals), so an impassable
        // one is never missed among collinear lines.
        if (line->flags & ML_BLOCKING)
        {
            return K_CROSS_BLOCKING;
        }
    }

    a = &sectors[from_sector];
    b = &sectors[to_sector];
    floor_a = a->floorheight;
    ceiling_a = a->ceilingheight;
    floor_b = b->floorheight;
    ceiling_b = b->ceilingheight;

    // Standing in a door means it is open.
    ceiling_a = OpenCeiling(a);

    if (to_sector == from_sector)
    {
        floor_b = floor_a;
        ceiling_b = ceiling_a;
    }

    // A lift too high now to step onto, or to fit over, that comes down by
    // itself (one that runs for good, or is on its way down): wait for it,
    // step on, ride it.
    if (lift_sectors[to_sector] && LiftComing(b)
     && (floor_b - floor_a > MAX_STEP_UP || !Headroom(floor_a, ceiling_a, floor_b, ceiling_b))
     && PlatOf(b)->low - floor_a <= MAX_STEP_UP
     && Headroom(floor_a, ceiling_a, PlatOf(b)->low, ceiling_b))
    {
        *wait = LiftArrival(b) + TurnWait(to_sector);
        return K_CROSS_LIFT;
    }

    // Stepping up is limited; dropping down never is (players have
    // MF_DROPOFF).
    if (floor_b - floor_a > MAX_STEP_UP)
    {
        fixed_t speed;
        fixed_t top = RisingTop(a, &speed);
        fixed_t off = floor_b - MAX_STEP_UP;

        // On a floor that will rise to within reach (a lift that is down
        // or coming back up, a floor on its way up, Doom II MAP03's strip
        // to the teleporter): wait on it. The player steps off as soon as
        // the step is low enough, so that is where it is judged; further
        // up there is only less headroom.
        if (off <= top && Headroom(off, ceiling_a, floor_b, ceiling_b))
        {
            const plat_t *plat = PlatOf(a);

            *wait = (double) (off - floor_a) / speed
                  + (plat != NULL && plat->status == waiting ? plat->count : 0)
                  + TurnWait(from_sector);
            return K_CROSS_RIDE;
        }

        // A raised lift whose switch is this line: lower it, step on, ride
        // it up.
        if (line != NULL && IsLiftSwitch(line->special) && line->tag != 0
         && line->tag == b->tag && line->frontsector == a
         && LiftBottom(b) - floor_a <= MAX_STEP_UP
         && Headroom(floor_a, ceiling_a, LiftBottom(b), ceiling_b))
        {
            fixed_t speed = IsBlazingLift(line->special) ? BLAZING_SPEED : LIFT_SPEED;

            *wait = ACTION_OVERHEAD + LIFT_WAIT
                  + 2.0 * (floor_b - LiftBottom(b)) / speed + TurnWait(to_sector);
            return K_CROSS_LIFT;
        }

        return K_CROSS_STEP;
    }

    if (!Headroom(floor_a, ceiling_a, floor_b, ceiling_b))
    {
        const vldoor_t *door = DoorOf(b);

        // On a lift too high now to fit under the way off, that comes down
        // by itself: ride it down.
        if (lift_sectors[from_sector] && LiftComing(a)
         && floor_b - PlatOf(a)->low <= MAX_STEP_UP
         && Headroom(PlatOf(a)->low, ceiling_a, floor_b, ceiling_b))
        {
            *wait = LiftArrival(a) + TurnWait(from_sector);
            return K_CROSS_RIDE;
        }

        // A door already opening (however it was opened; one-shot doors
        // lose their line special right away): wait for it, if it goes high
        // enough.
        if (door != NULL && door->direction == 1
         && Headroom(floor_a, ceiling_a, floor_b, SDL_max(ceiling_b, door->topheight)))
        {
            *wait = (double) SDL_max(0, floor_b + PLAYER_HEIGHT - ceiling_b) / door->speed;
            return K_CROSS_DOOR;
        }

        // A door opened by using this line from its front side, if it opens
        // high enough.
        if (line != NULL && IsManualDoor(line->special)
         && line->frontsector == a && line->backsector == b
         && Headroom(floor_a, ceiling_a, floor_b, SDL_max(ceiling_b, DoorTop(b))))
        {
            int key = K_Specials_LockKey(line->special);
            fixed_t speed = IsBlazingDoor(line->special) ? BLAZING_SPEED : DOOR_SPEED;

            if (key >= 0 && !K_Specials_HasKey(key))
            {
                return K_CROSS_LOCKED;
            }

            *wait = ACTION_OVERHEAD
                  + (double) SDL_max(0, floor_b + PLAYER_HEIGHT - ceiling_b) / speed;
            return K_CROSS_DOOR;
        }

        return K_CROSS_HEADROOM;
    }

    return K_CROSS_OPEN;
}

// The heights decide first; a crossing they let through is still no way
// if it is narrower than the player. An open one over a teleport line
// takes the player elsewhere. A ride is not judged for width now, low
// down, where the sides of the way off are walls (Doom II MAP03: the
// teleporter at the end of a rising strip, as wide as the player, between
// a closet's faces), but when it gets there.
static k_crossing_t Judge(int portal_index, int from, double *wait)
{
    k_crossing_t crossing = JudgeHeights(portal_index, from, wait);
    stance_t stance;

    if (!K_Graph_Passable(crossing) || from < 0 || crossing == K_CROSS_RIDE)
    {
        return crossing;
    }

    Stance(portal_index, from, &stance);

    if (Narrow(&stance))
    {
        *wait = 0.0;
        return K_CROSS_NARROW;
    }

    if (crossing == K_CROSS_OPEN && K_Graph_Teleport(portal_index, from) >= 0)
    {
        *wait = TELEPORT_FREEZE;
        return K_CROSS_TELEPORT;
    }

    return crossing;
}

k_crossing_t K_Graph_CanCross(int portal, int from)
{
    double wait;

    return Judge(portal, from, &wait);
}

k_vec2_t K_Graph_PortalPoint(int portal_index, int from)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &graph.portals[portal_index];
    double length = Length(Sub(portal->b, portal->a));
    int samples = SDL_min(MAX_WAYPOINT_SAMPLES,
                          SDL_max(2, (int) (length / WAYPOINT_STEP) + 1));
    int found[MAX_NEAR_LINES];
    double best_score = -1.0;
    k_vec2_t best = PointAlong(portal->a, portal->b, length * 0.5);
    stance_t stance;
    int count, s, i;

    Stance(portal_index, from, &stance);
    count = NearObstacles(&stance, WAYPOINT_CLEARANCE, found, MAX_NEAR_LINES);

    if (count == 0)
    {
        return best;
    }

    for (s = 0; s < samples; ++s)
    {
        double t = length * (s + 0.5) / samples;
        k_vec2_t at = PointAlong(portal->a, portal->b, t);
        double clearance = WAYPOINT_CLEARANCE;
        double score;

        for (i = 0; i < count; ++i)
        {
            clearance = SDL_min(clearance, SegmentDistance(map->lines[found[i]].v1,
                                                           map->lines[found[i]].v2, at));
        }

        // Clearance first, then nearness to the middle.
        score = clearance * 10000.0 - fabs(t - length * 0.5);

        if (score > best_score)
        {
            best_score = score;
            best = at;
        }
    }

    return best;
}

int K_Graph_NarrowSectors(int portal, int from, int *found_sectors, int max)
{
    const k_map_t *map = K_Map_Get();
    int lines_found[MAX_NEAR_LINES];
    stance_t stance;
    int count, found = 0;
    int i, side, j;

    Stance(portal, from, &stance);
    count = NearObstacles(&stance, CLEARANCE, lines_found, MAX_NEAR_LINES);

    for (i = 0; i < count; ++i)
    {
        const line_t *line = &lines[lines_found[i]];
        int past = StanceSide(&stance, &map->lines[lines_found[i]]);
        fixed_t floor = stance.floor[past], ceiling = stance.ceiling[past];

        if (line->backsector == NULL)
        {
            continue;
        }

        // The side that is too high or too low is what narrows the gap
        // (doors as in Obstacle).
        for (side = 0; side < 2; ++side)
        {
            sector_t *sector = side ? line->backsector : line->frontsector;
            int index = (int) (sector - sectors);
            fixed_t top = FaceCeiling(line, side, &stance.standing);

            if (sector->floorheight - floor <= MAX_STEP_UP
             && Headroom(floor, ceiling, sector->floorheight, top))
            {
                continue;
            }

            for (j = 0; j < found && found_sectors[j] != index; ++j)
            {
            }

            if (j == found && found < max)
            {
                found_sectors[found++] = index;
            }
        }
    }

    return found;
}

// Whether no point on the chord, nor CHORD_REACH past its ends, is
// CLEARANCE from every obstacle, for a player standing in the cell: as
// Narrow judges a crossing, sampled more finely. Once a tic.
static boolean ChordNarrow(int cell, chord_t *chord)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t dir = Sub(chord->b, chord->a);
    double length = Length(dir);
    int found[MAX_NEAR_LINES];
    k_portal_t across;
    stance_t stance;
    int count, samples, s, i;

    if (chord->judged == leveltime + 1)
    {
        return chord->narrow;
    }

    dir.x /= length;
    dir.y /= length;

    memset(&across, 0, sizeof(across));
    across.cell[0] = across.cell[1] = cell;
    across.a.x = chord->a.x - dir.x * CHORD_REACH;
    across.a.y = chord->a.y - dir.y * CHORD_REACH;
    across.b.x = chord->b.x + dir.x * CHORD_REACH;
    across.b.y = chord->b.y + dir.y * CHORD_REACH;
    across.line = -1;
    across.teleport_from = -1;
    length += 2.0 * CHORD_REACH;

    stance.portal = &across;
    stance.normal.x = -dir.y;
    stance.normal.y = dir.x;
    StandingHeights(cell, &stance.floor[0], &stance.ceiling[0]);
    stance.floor[1] = stance.floor[0];
    stance.ceiling[1] = stance.ceiling[0];
    stance.standing.sector[0] = map->subsectors[map->cells[cell].subsector].sector;
    stance.standing.sector[1] = stance.standing.sector[0];
    stance.standing.a = across.a;
    stance.standing.b = across.b;
    stance.standing.reach = CLEARANCE;
    stance.crossing = false;

    count = NearObstacles(&stance, CLEARANCE, found, MAX_NEAR_LINES);
    samples = (int) (length / CHORD_STEP) + 1;
    chord->narrow = true;

    for (s = 0; s <= samples && chord->narrow; ++s)
    {
        k_vec2_t at = PointAlong(across.a, across.b, length * s / samples);

        for (i = 0; i < count; ++i)
        {
            if (SegmentDistance(map->lines[found[i]].v1, map->lines[found[i]].v2, at)
                < CLEARANCE)
            {
                break;
            }
        }

        if (i == count)
        {
            chord->narrow = false;
        }
    }

    chord->judged = leveltime + 1;
    return chord->narrow;
}

// Which side of a chord's line a stretch of border lies on: 1 or -1, or 0
// if it reaches across. Its ends may touch the line.
static int ChordSide(const chord_t *chord, k_vec2_t a, k_vec2_t b)
{
    k_vec2_t dir = Sub(chord->b, chord->a);
    double length = Length(dir);
    double da = (dir.x * (a.y - chord->a.y) - dir.y * (a.x - chord->a.x)) / length;
    double db = (dir.x * (b.y - chord->a.y) - dir.y * (b.x - chord->a.x)) / length;
    int side_a = da > 0.5 ? 1 : da < -0.5 ? -1 : 0;
    int side_b = db > 0.5 ? 1 : db < -0.5 ? -1 : 0;

    if (side_a == 0 || side_b == 0)
    {
        return side_a + side_b;
    }

    return side_a == side_b ? side_a : 0;
}

int K_Graph_Pinched(int cell, k_vec2_t from_a, k_vec2_t from_b,
                    k_vec2_t to_a, k_vec2_t to_b, k_vec2_t *where)
{
    const k_map_t *map = K_Map_Get();
    int i;

    if (cell < 0 || chord_first == NULL || graph.generation != map->generation)
    {
        return false;
    }

    for (i = chord_first[cell]; i < chord_first[cell + 1]; ++i)
    {
        chord_t *chord = &chords[i];

        if (ChordSide(chord, from_a, from_b) * ChordSide(chord, to_a, to_b) < 0
         && ChordNarrow(cell, chord))
        {
            if (where != NULL)
            {
                where->x = (chord->a.x + chord->b.x) * 0.5;
                where->y = (chord->a.y + chord->b.y) * 0.5;
            }

            return true;
        }
    }

    return false;
}

int K_Graph_Passable(k_crossing_t crossing)
{
    return crossing == K_CROSS_OPEN || crossing == K_CROSS_DOOR
        || crossing == K_CROSS_LIFT || crossing == K_CROSS_RIDE
        || crossing == K_CROSS_TELEPORT;
}

int K_Graph_Teleport(int portal, int from)
{
    const k_portal_t *p = &graph.portals[portal];
    int teleport;

    if (from < 0 || p->teleport_from != from
     || !IsTeleportLine(lines[p->line].special))
    {
        return -1;
    }

    teleport = line_teleports[p->line];
    return graph.teleports[teleport].cell >= 0 ? teleport : -1;
}

double K_Graph_CrossingWait(int portal, int from)
{
    double wait;

    Judge(portal, from, &wait);
    return wait;
}

const char *K_Graph_CrossingName(k_crossing_t crossing)
{
    return crossing_names[crossing];
}

int K_Graph_LockKey(int portal)
{
    int line = graph.portals[portal].line;

    return line >= 0 ? K_Specials_LockKey(lines[line].special) : -1;
}

const char *K_Graph_LockName(int portal)
{
    int key = K_Graph_LockKey(portal);

    return key >= 0 ? K_Specials_KeyName(key) : NULL;
}

// The distance from a point to a segment as the engine's collisions see
// it: the player is a square, so the larger of the x and y distances. A
// wall corner level with the box's corner is 16 * sqrt(2) units away, but
// touching.
// The minimum of max(|u|, |v|), both linear along the segment, is at an
// end or where one of |u|, |v| bottoms out or they cross.
static double BoxDistance(k_vec2_t at, k_vec2_t a, k_vec2_t ab)
{
    double u0 = a.x - at.x, v0 = a.y - at.y;
    double candidates[6];
    double best = HUGE_VAL;
    int count = 0, i;

    candidates[count++] = 0.0;
    candidates[count++] = 1.0;

    if (fabs(ab.x) > 1e-12)
    {
        candidates[count++] = -u0 / ab.x;
    }
    if (fabs(ab.y) > 1e-12)
    {
        candidates[count++] = -v0 / ab.y;
    }
    if (fabs(ab.x - ab.y) > 1e-12)
    {
        candidates[count++] = (v0 - u0) / (ab.x - ab.y);
    }
    if (fabs(ab.x + ab.y) > 1e-12)
    {
        candidates[count++] = -(u0 + v0) / (ab.x + ab.y);
    }

    for (i = 0; i < count; ++i)
    {
        double t = SDL_max(0.0, SDL_min(1.0, candidates[i]));

        best = SDL_min(best, SDL_max(fabs(u0 + t * ab.x), fabs(v0 + t * ab.y)));
    }

    return best;
}

// K_Graph_ObstaclesNear for a player standing in the given cell.
static int ObstaclesAround(k_vec2_t at, int cell, double reach, k_nearline_t *found,
                           int max)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t min = { at.x - reach, at.y - reach };
    k_vec2_t max_ = { at.x + reach, at.y + reach };
    int lines_seen[MAX_NEAR_LINES];
    int seen = 0, count = 0;
    fixed_t floor, ceiling;
    standing_t standing;
    int x, y, x0, y0, x1, y1, k, i;

    if (cell < 0 || graph.generation != map->generation)
    {
        return 0;
    }

    StandingHeights(cell, &floor, &ceiling);
    standing.sector[0] = map->subsectors[map->cells[cell].subsector].sector;
    standing.sector[1] = -1;
    standing.a = standing.b = at;
    standing.reach = PLAYER_RADIUS;     // only a door the box is in
    CellRange(min, max_, &x0, &y0, &x1, &y1);

    for (y = y0; y <= y1; ++y)
    {
        for (x = x0; x <= x1; ++x)
        {
            int grid = y * grid_width + x;

            for (k = cell_first[grid]; k < cell_first[grid + 1]; ++k)
            {
                int line = cell_items[k] - num_edges;
                k_vec2_t a, ab, closest;
                double t, length, distance;

                if (line < 0 || count >= max)
                {
                    continue;
                }

                // A line spanning several grid cells comes up more than once.
                for (i = 0; i < seen && lines_seen[i] != line; ++i)
                {
                }

                if (i < seen)
                {
                    continue;
                }

                if (seen < MAX_NEAR_LINES)
                {
                    lines_seen[seen++] = line;
                }

                a = map->lines[line].v1;
                ab = Sub(map->lines[line].v2, a);
                t = SDL_max(0.0, SDL_min(1.0, Dot(Sub(at, a), ab)
                                              / SDL_max(Dot(ab, ab), 1e-12)));
                closest.x = a.x + ab.x * t;
                closest.y = a.y + ab.y * t;
                length = Length(Sub(at, closest));
                distance = BoxDistance(at, a, ab);

                if (distance >= reach || length < 1e-6
                 || !Obstacle(line, floor, ceiling, &standing))
                {
                    continue;
                }

                // Away from the nearest point: straight off a wall, round a
                // corner.
                found[count].away.x = (at.x - closest.x) / length;
                found[count].away.y = (at.y - closest.y) / length;
                found[count].distance = distance;
                ++count;
            }
        }
    }

    return count;
}

int K_Graph_ObstaclesNear(k_vec2_t at, double floor, double reach, k_nearline_t *found,
                          int max)
{
    return ObstaclesAround(at, K_Map_StandingCell(at.x, at.y, floor), reach, found, max);
}

double K_Graph_DropDistance(k_vec2_t at, double floor, double reach)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t min = { at.x - reach, at.y - reach };
    k_vec2_t max_ = { at.x + reach, at.y + reach };
    fixed_t standing = (fixed_t) (floor * FRACUNIT);
    double nearest = reach;
    int x, y, x0, y0, x1, y1, k;

    if (graph.generation != map->generation)
    {
        return reach;
    }

    CellRange(min, max_, &x0, &y0, &x1, &y1);

    for (y = y0; y <= y1; ++y)
    {
        for (x = x0; x <= x1; ++x)
        {
            int grid = y * grid_width + x;

            // A line spanning several grid cells comes up more than once,
            // which does not change the nearest.
            for (k = cell_first[grid]; k < cell_first[grid + 1]; ++k)
            {
                int line = cell_items[k] - num_edges;
                const line_t *l;
                fixed_t low, high;

                if (line < 0)
                {
                    continue;
                }

                l = &lines[line];

                if (l->backsector == NULL || (l->flags & ML_BLOCKING))
                {
                    continue;
                }

                low = SDL_min(l->frontsector->floorheight, l->backsector->floorheight);
                high = SDL_max(l->frontsector->floorheight, l->backsector->floorheight);

                if (standing - low <= MAX_STEP_UP || high - standing > MAX_STEP_UP)
                {
                    continue;
                }

                nearest = SDL_min(nearest, BoxDistance(at, map->lines[line].v1,
                                                       Sub(map->lines[line].v2,
                                                           map->lines[line].v1)));
            }
        }
    }

    return nearest;
}

double K_Graph_Clearance(k_vec2_t at, double reach)
{
    k_nearline_t found[MAX_NEAR_LINES];
    int cell = K_Map_CellAt(at.x, at.y);
    int count = ObstaclesAround(at, cell, reach, found, MAX_NEAR_LINES);
    double clearance = reach;
    int i;

    if (cell < 0)
    {
        return 0.0;
    }

    for (i = 0; i < count; ++i)
    {
        clearance = SDL_min(clearance, found[i].distance);
    }

    return clearance;
}

int K_Graph_MovingSector(void)
{
    const thinker_t *thinker;

    for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
    {
        actionf_p1 function = thinker->function.acp1;

        if (function == (actionf_p1) T_MoveFloor)
        {
            return (int) (((const floormove_t *) thinker)->sector - sectors);
        }

        if (function == (actionf_p1) T_VerticalDoor)
        {
            return (int) (((const vldoor_t *) thinker)->sector - sectors);
        }

        if (function == (actionf_p1) T_PlatRaise
         && ((const plat_t *) thinker)->type != perpetualRaise)
        {
            return (int) (((const plat_t *) thinker)->sector - sectors);
        }

        if (function == (actionf_p1) T_MoveCeiling)
        {
            const ceiling_t *ceiling = (const ceiling_t *) thinker;

            if (ceiling->type != crushAndRaise && ceiling->type != fastCrushAndRaise
             && ceiling->type != silentCrushAndRaise)
            {
                return (int) (ceiling->sector - sectors);
            }
        }
    }

    return -1;
}

k_mover_t K_Graph_SectorMover(int sector)
{
    const sector_t *s = &sectors[sector];
    const vldoor_t *door = DoorOf(s);
    const plat_t *plat = PlatOf(s);

    if (s->specialdata == NULL)
    {
        return K_MOVER_NONE;
    }

    if (door != NULL)
    {
        return door->direction == 1 ? K_MOVER_UP
             : door->direction == -1 ? K_MOVER_DOWN
             : door->direction == 0 ? K_MOVER_WAITING : K_MOVER_OTHER;
    }

    if (plat != NULL)
    {
        return plat->status == up ? K_MOVER_UP
             : plat->status == down ? K_MOVER_DOWN
             : plat->status == waiting ? K_MOVER_WAITING : K_MOVER_NONE;
    }

    return K_MOVER_OTHER;
}

int K_Graph_LiftTurnedBack(int sector)
{
    if (lift_turns != NULL && lift_turns[sector] < MAX_TURNS)
    {
        ++lift_turns[sector];
    }

    return lift_turns != NULL ? lift_turns[sector] : 0;
}

void K_Graph_LiftRidden(int sector)
{
    if (lift_turns != NULL)
    {
        lift_turns[sector] = 0;
    }
}
