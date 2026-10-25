//
// DESCRIPTION:
//	Karp pathfinder. A* runs over portal crossings rather than cells: a
//	route goes from the start through the middle of each portal it
//	crosses to the goal, and crossing a cell costs the straight line from
//	the portal it came in by to the one it leaves by. Both lie on the
//	border of the convex cell, so that line stays inside it. (Going
//	through each cell's centroid instead would detour deep into long
//	corridor cells and back.)
//
//	Crossings are judged live (K_Graph_CanCross). Blocked ones are not
//	skipped but cost a large penalty: when there is no open route, the
//	search still finds the one with the fewest blocked crossings, and the
//	first of them tells why the goal cannot be reached.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomtype.h"
#include "m_misc.h"

#include "k_path.h"

// Added per blocked crossing, unless the brain says otherwise. More than
// any open route on a map can cost, so a route with fewer blocked
// crossings always wins.
#define BLOCKED_PENALTY 1.0e7

// A door or lift costs the distance the player could run (16.7 units a
// tic) while waiting for it.
#define WAIT_COST 16.0

// A player standing on a hole in the mesh starts from the nearest cell
// within this distance.
#define START_SEARCH 64.0

// A leg between two portals on the same edge of a cell runs along that
// edge. That is common and harmless along a partition line, where the
// edge is all portals, but between two doorways in one wall it would
// scrape along the wall; such a leg goes through the cell's centroid.
#define BORDER_DISTANCE 1.0

typedef struct
{
    double f;
    int node;
} heapitem_t;

// Search nodes: node 2p + s is "crossed portal p into its cell[s]",
// followed by the start and the goal.
static int num_nodes;
static int nodes_capacity;
static double *cost;
static int *came_from;
static unsigned int *seen;      // == search: cost and came_from are valid
static unsigned int *done;      // == search: expanded
static unsigned int search;

static heapitem_t *heap;
static int heap_size;
static int heap_capacity;

static k_blockcost_t block_cost;

// The search in progress.
static const k_map_t *map;
static const k_graph_t *graph;
static k_vec2_t start_point, goal_point;
static int start_cell, goal_cell;
static int start_node, goal_node;

static const char *result_names[] =
{
    "found", "start off the floor", "goal off the floor", "blocked",
    "not connected",
};

static double Distance(k_vec2_t a, k_vec2_t b)
{
    return sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

static double SegmentDistance(k_vec2_t a, k_vec2_t b, k_vec2_t p)
{
    k_vec2_t ab = { b.x - a.x, b.y - a.y };
    double t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y)
             / SDL_max(ab.x * ab.x + ab.y * ab.y, 1e-12);
    k_vec2_t closest;

    t = SDL_max(0.0, SDL_min(1.0, t));
    closest.x = a.x + ab.x * t;
    closest.y = a.y + ab.y * t;
    return Distance(p, closest);
}

static k_vec2_t PortalMiddle(const k_graph_t *g, int portal)
{
    const k_portal_t *p = &g->portals[portal];
    k_vec2_t result = { (p->a.x + p->b.x) * 0.5, (p->a.y + p->b.y) * 0.5 };

    return result;
}

static k_vec2_t NodePosition(int node)
{
    if (node == start_node)
    {
        return start_point;
    }

    if (node == goal_node)
    {
        return goal_point;
    }

    return PortalMiddle(graph, node / 2);
}

// The cell a node is in: the one its portal leads into.
static int NodeCell(int node)
{
    if (node == start_node)
    {
        return start_cell;
    }

    if (node == goal_node)
    {
        return goal_cell;
    }

    return graph->portals[node / 2].cell[node % 2];
}

//
// Open set: a binary heap ordered by f, with stale entries skipped when
// popped.
//

static void Push(double f, int node)
{
    int i;

    if (heap_size == heap_capacity)
    {
        heap_capacity = SDL_max(heap_capacity * 2, 256);
        heap = realloc(heap, heap_capacity * sizeof(*heap));
    }

    for (i = heap_size++; i > 0; )
    {
        int parent = (i - 1) / 2;

        if (heap[parent].f <= f)
        {
            break;
        }

        heap[i] = heap[parent];
        i = parent;
    }

    heap[i].f = f;
    heap[i].node = node;
}

static int Pop(void)
{
    int node = heap[0].node;
    heapitem_t last = heap[--heap_size];
    int i = 0;

    for (;;)
    {
        int child = 2 * i + 1;

        if (child >= heap_size)
        {
            break;
        }

        if (child + 1 < heap_size && heap[child + 1].f < heap[child].f)
        {
            ++child;
        }

        if (heap[child].f >= last.f)
        {
            break;
        }

        heap[i] = heap[child];
        i = child;
    }

    heap[i] = last;
    return node;
}

//
// Search
//

static void PrepareNodes(void)
{
    num_nodes = 2 * graph->num_portals + 2;
    start_node = num_nodes - 2;
    goal_node = num_nodes - 1;

    if (num_nodes > nodes_capacity)
    {
        nodes_capacity = num_nodes;
        free(cost);
        free(came_from);
        free(seen);
        free(done);
        cost = malloc(nodes_capacity * sizeof(*cost));
        came_from = malloc(nodes_capacity * sizeof(*came_from));
        seen = calloc(nodes_capacity, sizeof(*seen));
        done = calloc(nodes_capacity, sizeof(*done));
        search = 0;
    }

    // The stamps tell this search's nodes from earlier ones'; start afresh
    // when the counter wraps.
    if (++search == 0)
    {
        memset(seen, 0, nodes_capacity * sizeof(*seen));
        memset(done, 0, nodes_capacity * sizeof(*done));
        search = 1;
    }

    heap_size = 0;
}

static void Relax(int node, int from, double step)
{
    double g = cost[from] + step;

    // The heuristic is consistent, so an expanded node is final.
    if (done[node] == search)
    {
        return;
    }

    if (seen[node] != search || g < cost[node])
    {
        seen[node] = search;
        cost[node] = g;
        came_from[node] = from;
        Push(g + Distance(NodePosition(node), goal_point), node);
    }
}

// The heuristic is the straight line to the goal, which no route can beat.
static boolean Search(k_path_t *path)
{
    seen[start_node] = search;
    cost[start_node] = 0.0;
    came_from[start_node] = -1;
    Push(Distance(start_point, goal_point), start_node);

    while (heap_size > 0)
    {
        int node = Pop();
        int cell, i, end;
        k_vec2_t at;

        if (done[node] == search)
        {
            continue;
        }

        done[node] = search;
        ++path->searched;

        if (node == goal_node)
        {
            return true;
        }

        cell = NodeCell(node);
        at = NodePosition(node);

        if (cell == goal_cell)
        {
            Relax(goal_node, node, Distance(at, goal_point));
        }

        end = graph->first_link[cell] + graph->link_count[cell];

        for (i = graph->first_link[cell]; i < end; ++i)
        {
            const k_link_t *link = &graph->links[i];
            const k_portal_t *portal = &graph->portals[link->portal];
            k_crossing_t crossing;
            double step;

            // Never straight back out the way it came in.
            if (node != start_node && link->portal == node / 2)
            {
                continue;
            }

            step = Distance(at, PortalMiddle(graph, link->portal));
            crossing = K_Graph_CanCross(link->portal, cell);

            if (!K_Graph_Passable(crossing))
            {
                step += block_cost != NULL ? block_cost(link->portal, cell, crossing)
                                           : BLOCKED_PENALTY;
            }
            else if (crossing != K_CROSS_OPEN)
            {
                step += WAIT_COST * K_Graph_CrossingWait(link->portal, cell);
            }

            Relax(2 * link->portal + (portal->cell[0] == link->neighbor ? 0 : 1),
                  node, step);
        }
    }

    return false;
}

//
// Results
//

static void EnsureCapacity(k_path_t *path, int cells)
{
    if (cells <= path->capacity)
    {
        return;
    }

    path->capacity = SDL_max(cells, path->capacity * 2);
    path->cells = realloc(path->cells, path->capacity * sizeof(*path->cells));
    path->portals = realloc(path->portals,
                            path->capacity * sizeof(*path->portals));

    // The start, the middle of each portal, at most one centroid per cell,
    // the goal.
    path->points = realloc(path->points,
                           (2 * path->capacity + 2) * sizeof(*path->points));
    path->leg_cells = realloc(path->leg_cells,
                              (2 * path->capacity + 2) * sizeof(*path->leg_cells));
}

static void Reconstruct(k_path_t *path)
{
    int count = 0;
    int node, i;

    // Portals crossed, counted back from the goal.
    for (node = came_from[goal_node]; node != start_node; node = came_from[node])
    {
        ++count;
    }

    EnsureCapacity(path, count + 1);
    path->num_cells = count + 1;
    path->cells[0] = start_cell;
    i = count;

    for (node = came_from[goal_node]; node != start_node; node = came_from[node])
    {
        --i;
        path->portals[i] = node / 2;
        path->cells[i + 1] = NodeCell(node);
    }
}

// Whether a point lies on a stretch of a cell's border that is not a
// portal: a wall.
static boolean OnWall(int cell, k_vec2_t point)
{
    const k_cell_t *c = &map->cells[cell];
    const k_vec2_t *poly = &map->points[c->first_point];
    boolean on_border = false;
    int i, end;

    for (i = 0; i < c->num_points && !on_border; ++i)
    {
        on_border = SegmentDistance(poly[i], poly[(i + 1) % c->num_points],
                                    point) < BORDER_DISTANCE;
    }

    if (!on_border)
    {
        return false;
    }

    end = graph->first_link[cell] + graph->link_count[cell];

    for (i = graph->first_link[cell]; i < end; ++i)
    {
        const k_portal_t *portal = &graph->portals[graph->links[i].portal];

        if (SegmentDistance(portal->a, portal->b, point) < BORDER_DISTANCE)
        {
            return false;
        }
    }

    return true;
}

// Sampled at a few points, enough to catch a wall between two doorways.
static boolean LegAlongWall(int cell, k_vec2_t a, k_vec2_t b)
{
    int i;

    for (i = 1; i <= 3; ++i)
    {
        k_vec2_t point = { a.x + (b.x - a.x) * i * 0.25,
                           a.y + (b.y - a.y) * i * 0.25 };

        if (OnWall(cell, point))
        {
            return true;
        }
    }

    return false;
}

static void BuildPoints(k_path_t *path)
{
    int i;

    path->num_points = 0;
    path->points[path->num_points++] = start_point;

    for (i = 0; i < path->num_cells - 1; ++i)
    {
        // Not quite the middle the search costed: as clear of walls as the
        // portal allows, so the player does not clip corners.
        k_vec2_t middle = K_Graph_PortalPoint(path->portals[i], path->cells[i]);

        // Between two portals (the first leg starts wherever the player
        // stands).
        if (i > 0 && LegAlongWall(path->cells[i],
                                  path->points[path->num_points - 1], middle))
        {
            path->leg_cells[path->num_points - 1] = i;
            path->points[path->num_points++] = map->cells[path->cells[i]].centroid;
        }

        path->leg_cells[path->num_points - 1] = i;
        path->points[path->num_points++] = middle;
    }

    path->leg_cells[path->num_points - 1] = path->num_cells - 1;
    path->points[path->num_points++] = goal_point;
    path->length = 0.0;

    for (i = 0; i < path->num_points - 1; ++i)
    {
        path->length += Distance(path->points[i], path->points[i + 1]);
    }
}

static void CellHeights(int cell, double *floor, double *ceiling)
{
    K_Map_SectorHeights(map->subsectors[map->cells[cell].subsector].sector,
                        floor, ceiling);
}

static void FindBlocked(k_path_t *path)
{
    int i;

    for (i = 0; i < path->num_cells - 1; ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);
        double from_floor, from_ceiling, to_floor, to_ceiling;

        if (K_Graph_Passable(crossing) || path->blocked_count++ > 0)
        {
            continue;
        }

        CellHeights(path->cells[i], &from_floor, &from_ceiling);
        CellHeights(path->cells[i + 1], &to_floor, &to_ceiling);

        path->result = K_PATH_BLOCKED;
        path->blocked_portal = path->portals[i];
        path->blocked_from = path->cells[i];
        path->blocked_by = crossing;

        if (crossing == K_CROSS_STEP)
        {
            path->blocked_height = to_floor - from_floor;
        }
        else if (crossing == K_CROSS_HEADROOM)
        {
            path->blocked_height = SDL_min(SDL_min(from_ceiling, to_ceiling)
                                         - SDL_max(from_floor, to_floor),
                                           to_ceiling - to_floor);
        }
    }
}

void K_Path_Find(k_path_t *path, k_vec2_t start, k_vec2_t goal)
{
    Uint64 begin = SDL_GetPerformanceCounter();

    map = K_Map_Get();
    graph = K_Graph_Get();

    path->start = start;
    path->goal = goal;
    path->num_cells = 0;
    path->num_points = 0;
    path->length = 0.0;
    path->blocked_portal = -1;
    path->blocked_from = -1;
    path->blocked_by = K_CROSS_OPEN;
    path->blocked_height = 0.0;
    path->blocked_count = 0;
    path->searched = 0;

    start_point = start;
    goal_point = goal;
    start_cell = goal_cell = -1;

    if (map->loaded && graph->generation == map->generation)
    {
        start_cell = K_Map_CellAt(start.x, start.y);

        if (start_cell < 0)
        {
            start_cell = K_Map_NearestCell(start.x, start.y, START_SEARCH);
        }

        goal_cell = K_Map_CellAt(goal.x, goal.y);
    }

    if (start_cell < 0)
    {
        path->result = K_PATH_NO_START;
    }
    else if (goal_cell < 0)
    {
        path->result = K_PATH_NO_GOAL;
    }
    else
    {
        PrepareNodes();

        if (Search(path))
        {
            path->result = K_PATH_FOUND;
            Reconstruct(path);
            BuildPoints(path);
            FindBlocked(path);
        }
        else
        {
            path->result = K_PATH_UNCONNECTED;
        }
    }

    path->search_ms = (SDL_GetPerformanceCounter() - begin) * 1000.0
                    / SDL_GetPerformanceFrequency();
}

void K_Path_SetBlockCost(k_blockcost_t cost)
{
    block_cost = cost;
}

void K_Path_Free(k_path_t *path)
{
    free(path->cells);
    free(path->portals);
    free(path->points);
    free(path->leg_cells);
    memset(path, 0, sizeof(*path));
}

const char *K_Path_ResultName(k_pathresult_t result)
{
    return result_names[result];
}

void K_Path_Describe(const k_path_t *path, char *buf, int buf_len)
{
    const k_graph_t *g = K_Graph_Get();
    char where[64];
    k_vec2_t at;
    int line;

    switch (path->result)
    {
        case K_PATH_FOUND:
            M_snprintf(buf, buf_len, "%d cells, %.0f units", path->num_cells,
                       path->length);
            return;

        case K_PATH_NO_START:
            M_snprintf(buf, buf_len, "the start (%.0f,%.0f) is not on any floor",
                       path->start.x, path->start.y);
            return;

        case K_PATH_NO_GOAL:
            M_snprintf(buf, buf_len, "the goal (%.0f,%.0f) is not on any floor",
                       path->goal.x, path->goal.y);
            return;

        case K_PATH_UNCONNECTED:
            M_snprintf(buf, buf_len, "nothing connects the start to the goal "
                       "(an area reached only by teleporter?)");
            return;

        case K_PATH_BLOCKED:
            break;
    }

    at = PortalMiddle(g, path->blocked_portal);
    line = g->portals[path->blocked_portal].line;

    if (line >= 0)
    {
        M_snprintf(where, sizeof(where), "line %d (%.0f,%.0f)", line, at.x, at.y);
    }
    else
    {
        M_snprintf(where, sizeof(where), "(%.0f,%.0f)", at.x, at.y);
    }

    switch (path->blocked_by)
    {
        case K_CROSS_STEP:
            M_snprintf(buf, buf_len, "a step %.0f units high at %s - the player "
                       "climbs at most 24", path->blocked_height, where);
            break;

        case K_CROSS_HEADROOM:
            M_snprintf(buf, buf_len, "only %.0f units of headroom at %s - the "
                       "player needs 56", path->blocked_height, where);
            break;

        case K_CROSS_BLOCKING:
            M_snprintf(buf, buf_len, "a line that blocks the player at %s", where);
            break;

        case K_CROSS_LOCKED:
            M_snprintf(buf, buf_len, "a door at %s that needs the %s key", where,
                       K_Graph_LockName(path->blocked_portal));
            break;

        case K_CROSS_NARROW:
            M_snprintf(buf, buf_len, "a gap narrower than the player at %s", where);
            break;

        default:
            M_snprintf(buf, buf_len, "a one-sided line at %s", where);
            break;
    }
}
