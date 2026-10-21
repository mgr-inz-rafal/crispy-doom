//
// DESCRIPTION:
//	Karp navigation graph. Two floor cells are neighbours where their
//	edges lie on the same line, run in opposite directions (all cells are
//	counter-clockwise) and overlap; each overlap becomes a portal. Edges
//	are bucketed in a uniform grid so large maps stay fast.
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
#include "m_fixed.h"
#include "r_state.h"

#include "k_graph.h"
#include "k_log.h"

// Vanilla nodebuilders round split vertices to whole map units, so the
// edges two neighbours share do not line up exactly: they can be off by
// up to this much in the middle of the shared stretch, and twice that at
// its ends (two slightly different angles drift apart). A thin wall would
// separate them by its full thickness along the whole stretch.
#define MATCH_TOLERANCE 1.0

// The shortest shared stretch that makes a portal.
#define MIN_OVERLAP 1.0

// A gap in a cell's border counts as a hole in the navigation mesh only if
// it is at least this long and this far from every wall. Shorter gaps are
// rounding at corners; gaps along a wall are hairline slivers of void.
#define MIN_HOLE 4.0
#define NEAR_WALL 2.0

#define GRID_CELL 128.0

// Player movement limits (p_map.c: P_TryMove, PIT_CheckLine).
#define MAX_STEP_UP (24 * FRACUNIT)
#define PLAYER_HEIGHT (56 * FRACUNIT)

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
};

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

static double Cross(k_vec2_t a, k_vec2_t b)
{
    return a.x * b.y - a.y * b.x;
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

// Distance of point p from the infinite line through a and b.
static double LineDistance(k_vec2_t a, k_vec2_t b, k_vec2_t p)
{
    return fabs(Cross(Sub(b, a), Sub(p, a))) / Length(Sub(b, a));
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
static boolean Overlap(k_vec2_t a, k_vec2_t b, k_vec2_t p, k_vec2_t q,
                       double *t0, double *t1)
{
    double length = Length(Sub(b, a));
    k_vec2_t dir;
    double tp, tq;

    if (length < MIN_OVERLAP || Length(Sub(q, p)) < MIN_OVERLAP)
    {
        return false;
    }

    dir.x = (b.x - a.x) / length;
    dir.y = (b.y - a.y) / length;
    tp = Dot(dir, Sub(p, a));
    tq = Dot(dir, Sub(q, a));
    *t0 = SDL_max(0.0, SDL_min(tp, tq));
    *t1 = SDL_min(length, SDL_max(tp, tq));

    return *t1 - *t0 >= MIN_OVERLAP
        && LineDistance(p, q, PointAlong(a, b, *t0)) <= 2.0 * MATCH_TOLERANCE
        && LineDistance(p, q, PointAlong(a, b, *t1)) <= 2.0 * MATCH_TOLERANCE
        && LineDistance(p, q, PointAlong(a, b, (*t0 + *t1) * 0.5))
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

// The stretch t0-t1 of edge e is shared with edge f.
static void AddPortal(int e, int f, double t0, double t1)
{
    k_portal_t *portal;
    double f0, f1;

    if (graph.num_portals == portals_capacity)
    {
        portals_capacity = SDL_max(portals_capacity * 2, 1024);
        graph.portals = realloc(graph.portals,
                                portals_capacity * sizeof(*graph.portals));
    }

    portal = &graph.portals[graph.num_portals++];
    portal->cell[0] = edges[e].cell;
    portal->cell[1] = edges[f].cell;
    portal->a = PointAlong(edges[e].a, edges[e].b, t0);
    portal->b = PointAlong(edges[e].a, edges[e].b, t1);
    portal->line = -1;

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

void K_Graph_Build(void)
{
    const k_map_t *map = K_Map_Get();
    Uint64 start = SDL_GetPerformanceCounter();
    int i;

    graph.num_portals = 0;
    num_covers = 0;

    CollectEdges(map);
    BuildGrid(map);
    MatchEdges(map);

    for (i = 0; i < graph.num_portals; ++i)
    {
        graph.portals[i].line = FindPortalLine(map, &graph.portals[i]);
    }

    BuildLinks(map);
    CheckHoles(map);
    CheckConnectivity(map);

    graph.generation = map->generation;
    graph.build_ms = (SDL_GetPerformanceCounter() - start) * 1000.0
                   / SDL_GetPerformanceFrequency();

    K_Log(K_LOG_MAP, graph.holes > 0 ? K_LOG_WARN : K_LOG_INFO,
          "%s graph: %d portals, %d holes (%.0f units), %d isolated, "
          "%d components (largest %d of %d cells), %.1f ms", map->name,
          graph.num_portals, graph.holes, graph.hole_length,
          graph.isolated_cells, graph.components, graph.largest_component,
          map->num_cells, graph.build_ms);
}

const k_graph_t *K_Graph_Get(void)
{
    return &graph;
}

k_crossing_t K_Graph_CanCross(int portal_index, int from)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &graph.portals[portal_index];
    int to = portal->cell[0] == from ? portal->cell[1] : portal->cell[0];
    int from_sector = map->subsectors[map->cells[from].subsector].sector;
    int to_sector = map->subsectors[map->cells[to].subsector].sector;
    const sector_t *a, *b;

    if (from_sector < 0 || to_sector < 0)
    {
        return K_CROSS_WALL;
    }

    if (portal->line >= 0)
    {
        const k_line_t *line = &map->lines[portal->line];

        if (line->back_sector < 0)
        {
            return K_CROSS_WALL;
        }

        if (line->flags & ML_BLOCKING)
        {
            return K_CROSS_BLOCKING;
        }
    }

    // Live heights: doors and lifts move.
    a = &sectors[from_sector];
    b = &sectors[to_sector];

    // Stepping up is limited; dropping down never is (players have
    // MF_DROPOFF).
    if (b->floorheight - a->floorheight > MAX_STEP_UP)
    {
        return K_CROSS_STEP;
    }

    if (SDL_min(a->ceilingheight, b->ceilingheight)
      - SDL_max(a->floorheight, b->floorheight) < PLAYER_HEIGHT
     || b->ceilingheight - b->floorheight < PLAYER_HEIGHT)
    {
        return K_CROSS_HEADROOM;
    }

    return K_CROSS_OPEN;
}

const char *K_Graph_CrossingName(k_crossing_t crossing)
{
    return crossing_names[crossing];
}
