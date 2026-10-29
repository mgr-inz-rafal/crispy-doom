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
//	A teleport line crossed from its front side leads to the teleporter's
//	destination instead of the cell beyond. The straight line to the goal
//	would then overestimate what is left, so the heuristic also allows for
//	flying to any teleporter and on from its destination: still never more
//	than any route costs, so A* still finds the shortest one.
//
//	A jump (k_jump.h) is a portal of its own, out of the cell its run-up
//	starts in: the route goes to the run-up's start, then straight to the
//	touchdown. It costs that distance and a little more, so a walk not much
//	longer wins; one that does not work now is a blocked crossing.
//
//	A floor that hurts costs the health it takes on the way (HEALTH_COST).
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

// Walking or waiting on a floor that hurts (nukage, blood) costs the health
// it takes at running speed, each point as much as HEALTH_COST units of
// running: a short wade beats a long way round. A route that would take
// more than HEALTH_SHARE of the player's health is planned again at
// HEALTH_COST_DEADLY a point, a tenth of what the brain charges for a
// blocked crossing it can fix: then any dry way round wins, and so does
// opening a door first (Doom II MAP20: the bot walked across the blood for
// half a minute and died, where the user's way is dry once switch 73 opens
// a door). The flight of a jump over such a floor costs nothing: the player
// is off it. Until the bot knows its armour and radiation suits.
#define HEALTH_COST 100.0
#define HEALTH_COST_DEADLY 1.0e5
#define HEALTH_SHARE 0.5
#define RUN_SPEED 16.7

// A player standing on a hole in the mesh starts from the nearest cell
// within this distance.
#define START_SEARCH 64.0

// A leg between two portals on the same edge of a cell runs along that
// edge. That is common and harmless along a partition line, where the
// edge is all portals, but between two doorways in one wall it would
// scrape along the wall, and along a ledge's edge (a floor more than
// LEDGE_HEIGHT lower) the player would fall off; such a leg goes through
// the cell's centroid.
#define BORDER_DISTANCE 1.0
#define LEDGE_HEIGHT 24.0

// The route goes this far past a teleport line, so the player crosses it.
#define TELEPORT_OVERSHOOT 32.0

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
static double *hurt;            // the health the floors take on the way
static int *came_from;
static unsigned int *seen;      // == search: cost and came_from are valid
static unsigned int *done;      // == search: expanded
static unsigned int search;

static heapitem_t *heap;
static int heap_size;
static int heap_capacity;

static k_blockcost_t block_cost;
static k_pinchcost_t pinch_cost;
static double health_cost;      // HEALTH_COST or HEALTH_COST_DEADLY

// The search in progress.
static const k_map_t *map;
static const k_graph_t *graph;
static k_vec2_t start_point, goal_point;
static int start_cell, goal_cell;
static int start_node, goal_node;

// Per teleporter: the heuristic at its destination.
static double *teleport_rest;
static int teleport_capacity;

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

// The teleporter a portal node arrives by, or -1: crossing a teleport line
// into its back side lands the player at the destination instead.
static int NodeTeleport(int node)
{
    return K_Graph_Teleport(node / 2, graph->portals[node / 2].cell[1 - node % 2]);
}

static k_vec2_t NodePosition(int node)
{
    int teleport;

    if (node == start_node)
    {
        return start_point;
    }

    if (node == goal_node)
    {
        return goal_point;
    }

    if (graph->portals[node / 2].jump >= 0)
    {
        return graph->portals[node / 2].b;
    }

    teleport = NodeTeleport(node);
    return teleport >= 0 ? graph->teleports[teleport].to : PortalMiddle(graph, node / 2);
}

// The cell a node is in: the one its portal leads into, or a teleporter's
// destination.
static int NodeCell(int node)
{
    int teleport;

    if (node == start_node)
    {
        return start_cell;
    }

    if (node == goal_node)
    {
        return goal_cell;
    }

    teleport = NodeTeleport(node);
    return teleport >= 0 ? graph->teleports[teleport].cell
                         : graph->portals[node / 2].cell[node % 2];
}

// Distance of point p from the box min-max (0 inside it).
static double BoxDistance(k_vec2_t min, k_vec2_t max, k_vec2_t p)
{
    double dx = SDL_max(0.0, SDL_max(min.x - p.x, p.x - max.x));
    double dy = SDL_max(0.0, SDL_max(min.y - p.y, p.y - max.y));

    return sqrt(dx * dx + dy * dy);
}

// The least a route from `at` to the goal can cost: the straight line, or
// a straight line to a teleporter's lines plus the least from its
// destination on, whichever is less. A route costs at least the straight
// line between any two of its points, and a teleporter is entered on its
// lines, so this never overestimates; and it changes by no more than the
// point moves, so it is consistent.
static double Heuristic(k_vec2_t at)
{
    double h = Distance(at, goal_point);
    int t;

    for (t = 0; t < graph->num_teleports; ++t)
    {
        const k_teleport_t *teleport = &graph->teleports[t];

        if (teleport->cell >= 0 && teleport_rest[t] < h)
        {
            h = SDL_min(h, BoxDistance(teleport->min, teleport->max, at)
                         + teleport_rest[t]);
        }
    }

    return h;
}

// The heuristic at each destination, which can itself go on by teleporter:
// relaxed until nothing changes (Bellman-Ford; a map has few teleporters).
static void PrepareTeleports(void)
{
    int t, round;

    if (graph->num_teleports > teleport_capacity)
    {
        teleport_capacity = graph->num_teleports;
        free(teleport_rest);
        teleport_rest = malloc(teleport_capacity * sizeof(*teleport_rest));
    }

    for (t = 0; t < graph->num_teleports; ++t)
    {
        teleport_rest[t] = Distance(graph->teleports[t].to, goal_point);
    }

    for (round = 0; round < graph->num_teleports; ++round)
    {
        boolean changed = false;

        for (t = 0; t < graph->num_teleports; ++t)
        {
            double h = Heuristic(graph->teleports[t].to);

            if (h < teleport_rest[t])
            {
                teleport_rest[t] = h;
                changed = true;
            }
        }

        if (!changed)
        {
            break;
        }
    }
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

// Where a node enters its cell: at the start, a teleporter's destination or
// a jump's touchdown (a point), else across its portal.
static void NodeEntry(int node, k_vec2_t *a, k_vec2_t *b)
{
    if (node == start_node || graph->portals[node / 2].jump >= 0 || NodeTeleport(node) >= 0)
    {
        *a = *b = NodePosition(node);
    }
    else
    {
        *a = graph->portals[node / 2].a;
        *b = graph->portals[node / 2].b;
    }
}

// A gap inside a cell (K_Graph_Pinched) is no crossing's: the brain judges
// it by the sectors that narrow it.
static double PinchCost(int cell, k_vec2_t at)
{
    return pinch_cost != NULL ? pinch_cost(cell, at) : BLOCKED_PENALTY;
}

// The health walking `distance` across a cell and waiting `wait` tics in it
// takes.
static double Damage(int cell, double distance, double wait)
{
    double per_tic = K_Map_FloorDamage(map->subsectors[map->cells[cell].subsector].sector);

    return per_tic > 0.0 ? (distance / RUN_SPEED + wait) * per_tic : 0.0;
}

static void PrepareNodes(void)
{
    num_nodes = 2 * graph->num_portals + 2;
    start_node = num_nodes - 2;
    goal_node = num_nodes - 1;

    if (num_nodes > nodes_capacity)
    {
        nodes_capacity = num_nodes;
        free(cost);
        free(hurt);
        free(came_from);
        free(seen);
        free(done);
        cost = malloc(nodes_capacity * sizeof(*cost));
        hurt = malloc(nodes_capacity * sizeof(*hurt));
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

// A step of the route, and the health its floor takes on the way.
static void Relax(int node, int from, double step, double damage)
{
    double g = cost[from] + step + damage * health_cost;

    // The heuristic is consistent, so an expanded node is final.
    if (done[node] == search)
    {
        return;
    }

    if (seen[node] != search || g < cost[node])
    {
        seen[node] = search;
        cost[node] = g;
        hurt[node] = hurt[from] + damage;
        came_from[node] = from;
        Push(g + Heuristic(NodePosition(node)), node);
    }
}

static boolean Search(k_path_t *path)
{
    PrepareTeleports();
    seen[start_node] = search;
    cost[start_node] = 0.0;
    hurt[start_node] = 0.0;
    came_from[start_node] = -1;
    Push(Heuristic(start_point), start_node);

    while (heap_size > 0)
    {
        int node = Pop();
        int cell, i, end;
        k_vec2_t at, entry_a, entry_b, pinch;

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
        NodeEntry(node, &entry_a, &entry_b);

        if (cell == goal_cell)
        {
            double step = Distance(at, goal_point);
            double damage = Damage(cell, step, 0.0);

            if (K_Graph_Pinched(cell, entry_a, entry_b, goal_point, goal_point, &pinch))
            {
                step += PinchCost(cell, pinch);
            }

            Relax(goal_node, node, step, damage);
        }

        end = graph->first_link[cell] + graph->link_count[cell];

        for (i = graph->first_link[cell]; i < end; ++i)
        {
            const k_link_t *link = &graph->links[i];
            const k_portal_t *portal = &graph->portals[link->portal];
            k_crossing_t crossing;
            double step, walk, wait = 0.0, beyond = 0.0;
            int next;

            // Never straight back out the way it came in.
            if (node != start_node && link->portal == node / 2)
            {
                continue;
            }

            crossing = K_Graph_CanCross(link->portal, cell);

            // A jump: to the run-up's start, then straight on to the
            // touchdown, after a ride if it starts from a lift that is down.
            // One that does not work now is blocked like any crossing: the
            // brain may know a line that moves its floors back.
            if (portal->jump >= 0)
            {
                walk = Distance(at, portal->a);
                step = walk + Distance(portal->a, portal->b);

                if (!K_Graph_Passable(crossing))
                {
                    step += block_cost != NULL ? block_cost(link->portal, cell, crossing)
                                               : BLOCKED_PENALTY;
                }
                else
                {
                    wait = K_Graph_CrossingWait(link->portal, cell, NULL);
                    step += WAIT_COST * wait;
                }

                if (K_Graph_Pinched(cell, entry_a, entry_b, portal->a, portal->a, &pinch))
                {
                    step += PinchCost(cell, pinch);
                }

                Relax(2 * link->portal + 1, node, step, Damage(cell, walk, wait));
                continue;
            }

            walk = Distance(at, PortalMiddle(graph, link->portal));
            step = walk;

            if (!K_Graph_Passable(crossing))
            {
                step += block_cost != NULL ? block_cost(link->portal, cell, crossing)
                                           : BLOCKED_PENALTY;
            }
            else if (crossing != K_CROSS_OPEN)
            {
                wait = K_Graph_CrossingWait(link->portal, cell, &beyond);
                step += WAIT_COST * wait;
            }

            // The way through the cell, from where the node came in.
            if (K_Graph_Pinched(cell, entry_a, entry_b, portal->a, portal->b, &pinch))
            {
                step += PinchCost(cell, pinch);
            }

            // Waiting here, then past the crossing (aboard a lift, frozen
            // at a teleporter's destination).
            next = 2 * link->portal + (portal->cell[0] == link->neighbor ? 0 : 1);
            Relax(next, node, step, Damage(cell, walk, wait - beyond)
                                    + Damage(NodeCell(next), 0.0, beyond));
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
    // past each teleporter and its destination, the goal.
    path->points = realloc(path->points,
                           (4 * path->capacity + 2) * sizeof(*path->points));
    path->leg_cells = realloc(path->leg_cells,
                              (4 * path->capacity + 2) * sizeof(*path->leg_cells));
    path->legs = realloc(path->legs, (4 * path->capacity + 2) * sizeof(*path->legs));
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

static void CellHeights(int cell, double *floor, double *ceiling);

// Whether a point lies on a stretch of a cell's border that is not a way
// on: a wall, a portal to a floor more than 24 higher or lower, or to a
// cell under a solid thing. Not a ledge's edge either: the player would
// fall off it (Doom II MAP16: both crossings of a landing on its open
// edge, the leg between them along it).
static boolean OnWall(int cell, k_vec2_t point)
{
    const k_cell_t *c = &map->cells[cell];
    const k_vec2_t *poly = &map->points[c->first_point];
    boolean on_border = false;
    double floor, ceiling;
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

    CellHeights(cell, &floor, &ceiling);
    end = graph->first_link[cell] + graph->link_count[cell];

    for (i = graph->first_link[cell]; i < end; ++i)
    {
        const k_portal_t *portal = &graph->portals[graph->links[i].portal];
        double neighbor_floor, neighbor_ceiling;

        CellHeights(graph->links[i].neighbor, &neighbor_floor, &neighbor_ceiling);

        if (portal->jump < 0
         && SegmentDistance(portal->a, portal->b, point) < BORDER_DISTANCE
         && fabs(neighbor_floor - floor) <= LEDGE_HEIGHT
         && K_Map_CellBlock(graph->links[i].neighbor) < 0)
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

// Adds a point; the leg to it is in route cell `cell`, taken as `leg`.
static void AddPoint(k_path_t *path, k_vec2_t point, int cell, k_leg_t leg)
{
    path->leg_cells[path->num_points - 1] = cell;
    path->legs[path->num_points - 1] = leg;
    path->points[path->num_points++] = point;
}

// Just past the teleport line a portal runs along: on its back side, the
// left of v1 -> v2.
static k_vec2_t PastLine(int portal, k_vec2_t at)
{
    const k_line_t *line = &map->lines[graph->portals[portal].line];
    double length = Distance(line->v1, line->v2);
    k_vec2_t result;

    result.x = at.x - (line->v2.y - line->v1.y) / length * TELEPORT_OVERSHOOT;
    result.y = at.y + (line->v2.x - line->v1.x) / length * TELEPORT_OVERSHOOT;
    return result;
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
        int teleport = K_Graph_Teleport(path->portals[i], path->cells[i]);

        // Between two portals (the first leg starts wherever the player
        // stands).
        if (i > 0 && LegAlongWall(path->cells[i],
                                  path->points[path->num_points - 1], middle))
        {
            AddPoint(path, map->cells[path->cells[i]].centroid, i, K_LEG_WALK);
        }

        AddPoint(path, middle, i, K_LEG_WALK);

        // A jump: from the run-up's start straight to the touchdown.
        if (graph->portals[path->portals[i]].jump >= 0)
        {
            AddPoint(path, graph->portals[path->portals[i]].b, i + 1, K_LEG_JUMP);
        }
        else if (teleport >= 0)
        {
            AddPoint(path, PastLine(path->portals[i], middle), i, K_LEG_WALK);
            AddPoint(path, graph->teleports[teleport].to, i + 1, K_LEG_TELEPORT);
        }
    }

    AddPoint(path, goal_point, path->num_cells - 1, K_LEG_WALK);
    path->legs[path->num_points - 1] = K_LEG_WALK;
    path->length = 0.0;

    for (i = 0; i < path->num_points - 1; ++i)
    {
        if (path->legs[i] != K_LEG_TELEPORT)
        {
            path->length += Distance(path->points[i], path->points[i + 1]);
        }
    }
}

static void CellHeights(int cell, double *floor, double *ceiling)
{
    K_Map_SectorHeights(map->subsectors[map->cells[cell].subsector].sector,
                        floor, ceiling);
    *floor = K_Map_CellFloor(cell);
}

// A gap on the way through route cell i: from the start, a teleporter's
// destination, a jump's touchdown or the portal in, to the portal out, a
// jump's run-up or the goal.
static boolean PinchedAt(const k_path_t *path, int i, k_vec2_t *where)
{
    k_vec2_t in_a, in_b, out_a, out_b;
    int teleport = i > 0 ? K_Graph_Teleport(path->portals[i - 1], path->cells[i - 1]) : -1;

    if (i == 0)
    {
        in_a = in_b = start_point;
    }
    else if (graph->portals[path->portals[i - 1]].jump >= 0)
    {
        in_a = in_b = graph->portals[path->portals[i - 1]].b;
    }
    else if (teleport >= 0)
    {
        in_a = in_b = graph->teleports[teleport].to;
    }
    else
    {
        in_a = graph->portals[path->portals[i - 1]].a;
        in_b = graph->portals[path->portals[i - 1]].b;
    }

    if (i == path->num_cells - 1)
    {
        out_a = out_b = goal_point;
    }
    else if (graph->portals[path->portals[i]].jump >= 0)
    {
        out_a = out_b = graph->portals[path->portals[i]].a;
    }
    else
    {
        out_a = graph->portals[path->portals[i]].a;
        out_b = graph->portals[path->portals[i]].b;
    }

    return K_Graph_Pinched(path->cells[i], in_a, in_b, out_a, out_b, where);
}

static void FindBlocked(k_path_t *path)
{
    int i;

    for (i = 0; i < path->num_cells; ++i)
    {
        const k_portal_t *portal;
        k_crossing_t crossing;
        double from_floor, from_ceiling, to_floor, to_ceiling;
        k_vec2_t where;

        if (PinchedAt(path, i, &where) && path->blocked_count++ == 0)
        {
            path->result = K_PATH_BLOCKED;
            path->blocked_portal = i < path->num_cells - 1 ? path->portals[i]
                                 : i > 0 ? path->portals[i - 1] : -1;
            path->blocked_from = path->cells[i];
            path->blocked_by = K_CROSS_NARROW;
            path->blocked_pinch = true;
            path->blocked_at = where;
        }

        if (i == path->num_cells - 1)
        {
            break;
        }

        portal = &graph->portals[path->portals[i]];
        crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (K_Graph_Passable(crossing) || path->blocked_count++ > 0)
        {
            continue;
        }

        // The cell beyond the portal: not cells[i + 1] past a teleporter.
        CellHeights(path->cells[i], &from_floor, &from_ceiling);
        CellHeights(portal->cell[portal->cell[0] == path->cells[i] ? 1 : 0],
                    &to_floor, &to_ceiling);

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

void K_Path_Find(k_path_t *path, k_vec2_t start, int from_cell, k_vec2_t goal)
{
    Uint64 begin = SDL_GetPerformanceCounter();

    map = K_Map_Get();
    graph = K_Graph_Get();

    path->start = start;
    path->goal = goal;
    path->num_cells = 0;
    path->num_points = 0;
    path->length = 0.0;
    path->damage = 0.0;
    path->blocked_portal = -1;
    path->blocked_from = -1;
    path->blocked_by = K_CROSS_OPEN;
    path->blocked_height = 0.0;
    path->blocked_count = 0;
    path->blocked_pinch = false;
    path->searched = 0;

    start_point = start;
    goal_point = goal;
    start_cell = goal_cell = -1;

    if (map->loaded && graph->generation == map->generation)
    {
        start_cell = from_cell >= 0 ? from_cell : K_Map_CellAt(start.x, start.y);

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
        boolean found;

        PrepareNodes();
        health_cost = HEALTH_COST;
        found = Search(path);

        // One that hurts too much: again, keeping off such floors.
        if (found && hurt[goal_node] > K_Map_PlayerHealth() * HEALTH_SHARE)
        {
            PrepareNodes();
            health_cost = HEALTH_COST_DEADLY;
            found = Search(path);
        }

        if (found)
        {
            path->result = K_PATH_FOUND;
            path->damage = hurt[goal_node];
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

void K_Path_SetPinchCost(k_pinchcost_t cost)
{
    pinch_cost = cost;
}

void K_Path_Free(k_path_t *path)
{
    free(path->cells);
    free(path->portals);
    free(path->points);
    free(path->leg_cells);
    free(path->legs);
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
            if (path->damage >= 0.5)
            {
                M_snprintf(buf, buf_len, "%d cells, %.0f units, about %.0f health off floors "
                           "that hurt", path->num_cells, path->length, path->damage);
            }
            else
            {
                M_snprintf(buf, buf_len, "%d cells, %.0f units", path->num_cells,
                           path->length);
            }
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
            M_snprintf(buf, buf_len, "nothing connects the start to the goal, "
                       "no portal nor teleporter");
            return;

        case K_PATH_BLOCKED:
            break;
    }

    if (path->blocked_pinch)
    {
        M_snprintf(buf, buf_len, "a gap narrower than the player at (%.0f,%.0f)",
                   path->blocked_at.x, path->blocked_at.y);
        return;
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

        case K_CROSS_THING:
        {
            const k_portal_t *portal = &g->portals[path->blocked_portal];
            int block = K_Map_CellBlock(portal->cell[0] == path->blocked_from
                                        ? portal->cell[1] : portal->cell[0]);
            const k_block_t *b = block >= 0 ? &K_Map_Get()->blocks[block] : NULL;

            if (b != NULL)
            {
                M_snprintf(buf, buf_len, "%s at (%.0f,%.0f) in the way",
                           b->barrel ? "a barrel" : "a solid thing", b->at.x, b->at.y);
            }
            else
            {
                M_snprintf(buf, buf_len, "a solid thing in the way at %s", where);
            }

            break;
        }

        case K_CROSS_GAP:
            M_snprintf(buf, buf_len, "a jump at %s that does not work now", where);
            break;

        default:
            M_snprintf(buf, buf_len, "a one-sided line at %s", where);
            break;
    }
}
