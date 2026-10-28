//
// DESCRIPTION:
//	Karp jumps (k_jump.h). The run is the engine's movement, replayed on
//	Karp's own data so nothing in the game is touched (plan.md, 2.4):
//	P_MovePlayer's thrust, P_XYMovement's moves in halves and friction,
//	P_ZMovement's gravity, P_TryMove's checks, with P_CheckPosition's floor
//	and ceiling under the box from the lines it straddles.
//
//	Takeoff edges are the portals the floor drops more than 24 across.
//	From points along each, rays go out straight and to either side; each
//	gets as long a run-up as the takeoff floor allows behind the edge, and
//	the run decides: landed on a floor more than 24 above the lowest one
//	passed over (one the player could not climb to from there), fell in,
//	or ran into something. Whatever lies in between - one pit, a sidewalk,
//	a fence and water - is crossed as the engine would. Landed is not
//	enough: braking at once (as nav does), the player must come to a stop
//	before it slides off again (Doom II MAP24: a walkway 16 wide over the
//	nukage, slid across at 13 units a tic). Touching down where it could
//	not stop, it runs on, as the engine carries it: Doom II MAP06's jump
//	from a lift to the blue key hops off a pillar's top 16 square, level
//	with the lift, 8 units out.
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
#include "k_jump.h"
#include "k_log.h"

#define K_PI 3.14159265358979323846

// Player movement (p_user.c, p_mobj.c, p_map.c), in map units: forward 50
// thrusts 50 * 2048 / 65536 a tic; friction on the ground only; momentum
// capped at MAX_MOVE a tic, and moved in halves above half that; gravity
// of a unit a tic, two on the first; steps of 24, headroom of 56.
#define THRUST (50.0 * 2048.0 / 65536.0)
#define FRICTION (0xe800 / 65536.0)
#define MAX_MOVE 30.0
#define PLAYER_RADIUS 16.0
#define PLAYER_HEIGHT 56.0
#define MAX_STEP 24.0

// The run-up: as far back from the edge as the takeoff floor goes, clear,
// up to RUNUP_MAX, checked every RUNUP_STEP; none at all may do. A gap
// narrower than the player needs no speed (Doom II MAP08: a strip of slime
// 17 wide behind the yellow door, which leaves 22 units to take off from).
// A short run-up of RUNUP_SHORT is tried too.
#define RUNUP_MAX 128.0
#define RUNUP_STEP 8.0
#define RUNUP_SHORT 8.0

// Takeoff points along an edge, one per SAMPLE_SPACING; rays straight out
// and RAY_ANGLE degrees to either side.
#define SAMPLE_SPACING 48.0
#define RAY_ANGLE 25.0

// The margin (the user: no jump that needs near-perfect conditions): it
// must still land with MARGIN_THRUST of the thrust, and from starts
// MARGIN_SIDE to either side.
#define MARGIN_THRUST 0.85
#define MARGIN_SIDE 6.0

// A run gives up after this many tics (about 1100 units at full speed).
#define MAX_TICS 70

// After touching down, the player coasts for LAND_COAST tics (the bot's
// commands for them were built in the air, with no thrust), then brakes,
// and is stopped below STOP_SPEED, within STOP_TICS.
#define LAND_COAST 2
#define STOP_SPEED 1.0
#define STOP_TICS 35

#define MAX_LINES 128
#define MAX_BLOCKS 32

// Only this many jumps are logged one by one.
#define MAX_REPORTS 12

typedef enum
{
    RUN_LANDED,             // stopped on a floor it could not have climbed to
    RUN_FELL,               // back down on the floors it passed over
    RUN_BLOCKED,            // a wall, a solid thing, a step too high, no
                            // headroom
    RUN_TIMEOUT,
    RUN_BAD_START,          // the box does not fit at the start
} runresult_t;

typedef struct
{
    runresult_t result;
    k_vec2_t edge;          // where the last tic on the takeoff floor began
    double speed;           // the move of that tic
    k_vec2_t land;
    double land_floor;
    int land_sector;
} run_t;

typedef struct
{
    double x, y, z;
    double momx, momy, momz;
    double floorz, ceilingz;
} body_t;

static k_jump_t *jumps;
static int num_jumps;
static int jumps_capacity;
static int fixed_jumps;             // these are in the graph already: kept
static int *judged;                 // per jump: leveltime + 1 when judged
static unsigned char *possible;
static boolean doors_open;          // manual doors count as open
                                    // (CheckPosition)

static double Height(fixed_t height)
{
    return height / (double) FRACUNIT;
}

static sector_t *SectorAt(double x, double y)
{
    return R_PointInSubsector((fixed_t) (x * FRACUNIT), (fixed_t) (y * FRACUNIT))->sector;
}

static int CellSector(int cell)
{
    const k_map_t *map = K_Map_Get();

    return map->subsectors[map->cells[cell].subsector].sector;
}

static double CellFloor(int cell)
{
    double floor, ceiling;

    K_Map_SectorHeights(CellSector(cell), &floor, &ceiling);
    return floor;
}

// A sector's ceiling for the run: a manual door at its open height while
// doors_open.
static double Ceiling(const sector_t *sector)
{
    int index = (int) (sector - sectors);

    return doors_open && K_Graph_IsDoor(index) ? K_Graph_DoorCeiling(index)
                                               : Height(sector->ceilingheight);
}

// Whether the box from (x0, y0) to (x1, y1) straddles the line through the
// segment (P_BoxOnLineSide == -1).
static boolean BoxStraddles(double x0, double y0, double x1, double y1, const k_line_t *line)
{
    double dx = line->v2.x - line->v1.x, dy = line->v2.y - line->v1.y;
    boolean left = false, right = false;
    int k;

    for (k = 0; k < 4; ++k)
    {
        double px = (k & 1) ? x1 : x0, py = (k & 2) ? y1 : y0;
        double cross = dx * (py - line->v1.y) - dy * (px - line->v1.x);

        left |= cross > 1e-9;
        right |= cross < -1e-9;
    }

    return left && right;
}

// P_CheckPosition for the player's box at (x, y), live: false if a wall, an
// impassable line or a standing solid thing is in the way; else the floor
// and ceiling the box gets, from the sector under its centre and every
// two-sided line it straddles.
static boolean CheckPosition(double x, double y, double *floorz, double *ceilingz)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t min = { x - PLAYER_RADIUS, y - PLAYER_RADIUS };
    k_vec2_t max = { x + PLAYER_RADIUS, y + PLAYER_RADIUS };
    int found[MAX_LINES];
    int blocks[MAX_BLOCKS];
    sector_t *sector = SectorAt(x, y);
    int count, i;

    *floorz = Height(sector->floorheight);
    *ceilingz = Ceiling(sector);

    count = K_Map_BlocksNear(min, max, blocks, MAX_BLOCKS);

    for (i = 0; i < count; ++i)
    {
        const k_block_t *block = &map->blocks[blocks[i]];
        double reach = block->radius + PLAYER_RADIUS;

        if (fabs(block->at.x - x) < reach && fabs(block->at.y - y) < reach)
        {
            return false;
        }
    }

    count = K_Graph_LinesNear(min, max, found, MAX_LINES);

    for (i = 0; i < count; ++i)
    {
        const k_line_t *kline = &map->lines[found[i]];
        const line_t *line = &lines[found[i]];

        if (max.x <= SDL_min(kline->v1.x, kline->v2.x) || min.x >= SDL_max(kline->v1.x, kline->v2.x)
         || max.y <= SDL_min(kline->v1.y, kline->v2.y) || min.y >= SDL_max(kline->v1.y, kline->v2.y)
         || !BoxStraddles(min.x, min.y, max.x, max.y, kline))
        {
            continue;
        }

        if (line->backsector == NULL || (line->flags & ML_BLOCKING))
        {
            return false;
        }

        *ceilingz = SDL_min(*ceilingz, SDL_min(Ceiling(line->frontsector),
                                               Ceiling(line->backsector)));
        *floorz = SDL_max(*floorz, Height(SDL_max(line->frontsector->floorheight,
                                                  line->backsector->floorheight)));
    }

    return true;
}

// P_TryMove's checks for a player.
static boolean TryMove(body_t *body, double x, double y)
{
    double floorz, ceilingz;

    if (!CheckPosition(x, y, &floorz, &ceilingz)
     || ceilingz - floorz < PLAYER_HEIGHT
     || ceilingz - body->z < PLAYER_HEIGHT
     || floorz - body->z > MAX_STEP)
    {
        return false;
    }

    body->x = x;
    body->y = y;
    body->floorz = floorz;
    body->ceilingz = ceilingz;
    return true;
}

// One tic of the player's movement: the thrust (on the ground only), the
// move in halves while more than half of MAX_MOVE (only a positive move is
// checked, as in the engine), friction on the ground, gravity. False if a
// move is stopped.
static boolean Tic(body_t *body, double thrust_x, double thrust_y)
{
    double xmove, ymove;

    if (body->z <= body->floorz)
    {
        body->momx += thrust_x;
        body->momy += thrust_y;
    }

    body->momx = SDL_max(-MAX_MOVE, SDL_min(body->momx, MAX_MOVE));
    body->momy = SDL_max(-MAX_MOVE, SDL_min(body->momy, MAX_MOVE));
    xmove = body->momx;
    ymove = body->momy;

    do
    {
        double tx, ty;

        if (xmove > MAX_MOVE / 2 || ymove > MAX_MOVE / 2)
        {
            tx = body->x + xmove / 2;
            ty = body->y + ymove / 2;
            xmove /= 2;
            ymove /= 2;
        }
        else
        {
            tx = body->x + xmove;
            ty = body->y + ymove;
            xmove = ymove = 0.0;
        }

        if (!TryMove(body, tx, ty))
        {
            return false;
        }
    } while (xmove != 0.0 || ymove != 0.0);

    if (body->z <= body->floorz)
    {
        body->momx *= FRICTION;
        body->momy *= FRICTION;
    }

    body->z += body->momz;

    if (body->z <= body->floorz)
    {
        body->z = body->floorz;
        body->momz = SDL_max(body->momz, 0.0);
    }
    else
    {
        body->momz = body->momz == 0.0 ? -2.0 : body->momz - 1.0;
    }

    if (body->z + PLAYER_HEIGHT > body->ceilingz)
    {
        body->momz = SDL_min(body->momz, 0.0);
        body->z = body->ceilingz - PLAYER_HEIGHT;
    }

    return true;
}

// Whether a player just touched down on `floor` stops on it: coasting
// LAND_COAST tics, then thrusting against its momentum, until it is slow
// enough (or a wall stops it) before its box leaves the floor.
static boolean StopsOn(body_t body, double floor)
{
    int tic;

    for (tic = 0; tic < STOP_TICS; ++tic)
    {
        double speed = sqrt(body.momx * body.momx + body.momy * body.momy);
        double brake = tic < LAND_COAST || speed < 1e-9 ? 0.0 : SDL_min(speed, THRUST) / speed;

        if (body.z <= body.floorz && speed < STOP_SPEED)
        {
            return true;
        }

        if (!Tic(&body, -body.momx * brake, -body.momy * brake))
        {
            return true;
        }

        if (body.floorz < floor - MAX_STEP)
        {
            return false;
        }
    }

    return false;
}

// A straight run from a standing start, thrusting `thrust` a tic along dir
// whenever on the ground (the bot holds forward, or less), until it lands,
// falls, is stopped, or MAX_TICS run out. Manual doors count as open while
// it is on the takeoff floor: the box can overlap a door it came through,
// and it holds that door open (Doom II MAP08: the hop onto the first
// sinking pillar, off a strip 8 units wide behind the red door).
static void Run(k_vec2_t start, k_vec2_t dir, double thrust, run_t *run)
{
    body_t body;
    double takeoff, lowest;
    int tic;

    memset(run, 0, sizeof(*run));
    run->land_sector = -1;
    run->edge = start;
    doors_open = true;

    if (!CheckPosition(start.x, start.y, &body.floorz, &body.ceilingz)
     || body.ceilingz - body.floorz < PLAYER_HEIGHT)
    {
        run->result = RUN_BAD_START;
        doors_open = false;
        return;
    }

    body.x = start.x;
    body.y = start.y;
    body.z = body.floorz;
    body.momx = body.momy = body.momz = 0.0;
    takeoff = body.floorz;
    lowest = takeoff;           // the lowest floor under the centre so far

    for (tic = 0; tic < MAX_TICS; ++tic)
    {
        sector_t *under;

        doors_open = body.z <= body.floorz && fabs(body.floorz - takeoff) < 0.01;

        if (doors_open)
        {
            run->edge.x = body.x;
            run->edge.y = body.y;
            run->speed = hypot(body.momx + dir.x * thrust, body.momy + dir.y * thrust);
        }

        if (!Tic(&body, dir.x * thrust, dir.y * thrust))
        {
            run->result = RUN_BLOCKED;
            doors_open = false;
            return;
        }

        doors_open = false;

        under = SectorAt(body.x, body.y);
        lowest = SDL_min(lowest, Height(under->floorheight));

        // On the ground, its centre over the floor it stands on: landed if
        // that is too high to climb to from the lowest floor passed over,
        // and it stops there; if it cannot stop, it runs on (a pillar's top
        // on the way). Fell if it is down among them. Over an edge, the box
        // still stands on the floor it is leaving.
        if (body.z <= body.floorz && fabs(Height(under->floorheight) - body.floorz) < 0.01)
        {
            if (body.floorz > lowest + MAX_STEP)
            {
                if (StopsOn(body, body.floorz))
                {
                    run->result = RUN_LANDED;
                    run->land.x = body.x;
                    run->land.y = body.y;
                    run->land_floor = body.floorz;
                    run->land_sector = (int) (under - sectors);
                    return;
                }
            }
            else if (lowest < takeoff - MAX_STEP)
            {
                run->result = RUN_FELL;
                return;
            }
        }
    }

    run->result = RUN_TIMEOUT;
}

// Whether a run lands where the jump does.
static boolean Lands(const run_t *run, int sector)
{
    return run->result == RUN_LANDED && run->land_sector == sector;
}

// The jump off the edge at `at`, along dir, with a run-up of `back`, from
// the floor it drops from: kept if it lands with the margin, and is the
// shortest so far between its cells (one already in the graph stays as it
// is).
static void TryRunUp(k_vec2_t at, k_vec2_t dir, double back, double from_floor)
{
    k_vec2_t side = { -dir.y, dir.x };
    k_vec2_t start = { at.x - dir.x * back, at.y - dir.y * back };
    run_t run, check;
    int from_cell, to_cell, i, k;
    k_jump_t *jump;

    Run(start, dir, THRUST, &run);

    if (run.result != RUN_LANDED)
    {
        return;
    }

    from_cell = K_Map_StandingCell(start.x, start.y, from_floor);
    to_cell = K_Map_StandingCell(run.land.x, run.land.y, run.land_floor);

    if (from_cell < 0 || to_cell < 0 || from_cell == to_cell)
    {
        return;
    }

    Run(start, dir, THRUST * MARGIN_THRUST, &check);

    if (!Lands(&check, run.land_sector))
    {
        return;
    }

    for (k = -1; k <= 1; k += 2)
    {
        k_vec2_t shifted = { start.x + side.x * k * MARGIN_SIDE,
                             start.y + side.y * k * MARGIN_SIDE };

        Run(shifted, dir, THRUST, &check);

        if (!Lands(&check, run.land_sector))
        {
            return;
        }
    }

    for (i = 0; i < num_jumps; ++i)
    {
        if (jumps[i].from_cell == from_cell && jumps[i].to_cell == to_cell)
        {
            break;
        }
    }

    if (i < num_jumps)
    {
        if (i < fixed_jumps
         || hypot(run.land.x - start.x, run.land.y - start.y)
            >= hypot(jumps[i].land.x - jumps[i].start.x, jumps[i].land.y - jumps[i].start.y))
        {
            return;
        }
    }
    else
    {
        if (num_jumps == jumps_capacity)
        {
            jumps_capacity = SDL_max(jumps_capacity * 2, 64);
            jumps = realloc(jumps, jumps_capacity * sizeof(*jumps));
        }

        i = num_jumps++;
    }

    jump = &jumps[i];
    jump->from_cell = from_cell;
    jump->to_cell = to_cell;
    jump->start = start;
    jump->edge = run.edge;
    jump->land = run.land;
    jump->dir = dir;
    jump->speed = run.speed;
    jump->flight = hypot(run.land.x - run.edge.x, run.land.y - run.edge.y);
    jump->from_floor = from_floor;
    jump->land_floor = run.land_floor;
    jump->land_sector = run.land_sector;
}

// The jumps off the edge at `at`, along dir, from the floor and sector it
// drops from: with the longest run-up there is room for, and with a short
// one, which a gap that needs little speed takes without first backing up
// (Doom II MAP08: pillars that sink once stepped on).
static void TryJump(k_vec2_t at, k_vec2_t dir, double from_floor, int from_sector)
{
    double back = 0.0;
    double floorz, ceilingz;

    // The run-up, behind the edge on the takeoff floor (doors open, as in a
    // run). Its centre never crosses a door's side: nav would not know to
    // open the door on the way.
    doors_open = true;

    while (back + RUNUP_STEP <= RUNUP_MAX)
    {
        double x = at.x - dir.x * (back + RUNUP_STEP);
        double y = at.y - dir.y * (back + RUNUP_STEP);
        sector_t *sector = SectorAt(x, y);
        int index = (int) (sector - sectors);

        if (!CheckPosition(x, y, &floorz, &ceilingz) || fabs(floorz - from_floor) > 0.01
         || ceilingz - floorz < PLAYER_HEIGHT
         || fabs(Height(sector->floorheight) - from_floor) > 0.01
         || (index != from_sector && (K_Graph_IsDoor(index) || K_Graph_IsDoor(from_sector))))
        {
            break;
        }

        back += RUNUP_STEP;
    }

    doors_open = false;

    TryRunUp(at, dir, back, from_floor);

    if (back > RUNUP_SHORT)
    {
        TryRunUp(at, dir, RUNUP_SHORT, from_floor);
    }
}

// Whether a point is in the box (min, max).
static boolean InBox(k_vec2_t p, const k_vec2_t *min, const k_vec2_t *max)
{
    return p.x >= min->x && p.x <= max->x && p.y >= min->y && p.y <= max->y;
}

// Tries the jumps off every edge the floor drops more than 24 across (the
// shared stretches of border, not jumps), or only those with an end in the
// box (min, max) if one is given. Returns how many edges were tried.
static int SearchEdges(const k_vec2_t *min, const k_vec2_t *max)
{
    const k_graph_t *graph = K_Graph_Get();
    const k_map_t *map = K_Map_Get();
    int edges = 0;
    int p, side, s, a;

    for (p = 0; p < graph->num_portals; ++p)
    {
        const k_portal_t *portal = &graph->portals[p];
        k_vec2_t along = { portal->b.x - portal->a.x, portal->b.y - portal->a.y };
        double length = sqrt(along.x * along.x + along.y * along.y);

        if (length < 1.0 || portal->jump >= 0
         || (min != NULL && !InBox(portal->a, min, max) && !InBox(portal->b, min, max)))
        {
            continue;
        }

        for (side = 0; side < 2; ++side)
        {
            int from = portal->cell[side], to = portal->cell[1 - side];
            double from_floor = CellFloor(from);
            k_vec2_t normal = { -along.y / length, along.x / length };
            const k_vec2_t *inside = &map->cells[to].centroid;
            int samples;

            if (from_floor - CellFloor(to) <= MAX_STEP)
            {
                continue;
            }

            // Pointing over the drop.
            if (normal.x * (inside->x - portal->a.x) + normal.y * (inside->y - portal->a.y) < 0.0)
            {
                normal.x = -normal.x;
                normal.y = -normal.y;
            }

            ++edges;
            samples = SDL_max(1, (int) (length / SAMPLE_SPACING + 0.5));

            for (s = 0; s < samples; ++s)
            {
                double t = (s + 0.5) / samples;
                k_vec2_t at = { portal->a.x + along.x * t, portal->a.y + along.y * t };

                for (a = -1; a <= 1; ++a)
                {
                    double angle = a * RAY_ANGLE * K_PI / 180.0;
                    k_vec2_t dir = { normal.x * cos(angle) - normal.y * sin(angle),
                                     normal.x * sin(angle) + normal.y * cos(angle) };

                    TryJump(at, dir, from_floor, CellSector(from));
                }
            }
        }
    }

    return edges;
}

// Room in the per-jump judging arrays for every jump, the new ones unjudged.
static void GrowJudging(int old_count)
{
    judged = realloc(judged, SDL_max(num_jumps, 1) * sizeof(*judged));
    possible = realloc(possible, SDL_max(num_jumps, 1));
    memset(judged + old_count, 0, (num_jumps - old_count) * sizeof(*judged));
    memset(possible + old_count, 0, num_jumps - old_count);
}

static void LogJump(int index)
{
    const k_jump_t *jump = &jumps[index];

    K_Log(K_LOG_MAP, K_LOG_INFO, "jump from (%.0f,%.0f) cell %d to (%.0f,%.0f) cell %d: "
          "%.0f units off the edge at %.1f units a tic", jump->start.x, jump->start.y,
          jump->from_cell, jump->land.x, jump->land.y, jump->to_cell, jump->flight,
          jump->speed);
}

void K_Jump_Find(void)
{
    const k_map_t *map = K_Map_Get();
    Uint64 begin = SDL_GetPerformanceCounter();
    int edges, p;

    num_jumps = 0;
    fixed_jumps = 0;
    edges = SearchEdges(NULL, NULL);

    free(judged);
    free(possible);
    judged = NULL;
    possible = NULL;
    GrowJudging(0);

    for (p = 0; p < num_jumps && p < MAX_REPORTS; ++p)
    {
        LogJump(p);
    }

    K_Log(K_LOG_MAP, K_LOG_INFO, "%s: %d jumps off %d edges, %.1f ms", map->name, num_jumps,
          edges, (SDL_GetPerformanceCounter() - begin) * 1000.0 / SDL_GetPerformanceFrequency());
}

int K_Jump_FindNear(k_vec2_t min, k_vec2_t max)
{
    Uint64 begin = SDL_GetPerformanceCounter();
    int edges, p;

    fixed_jumps = num_jumps;
    edges = SearchEdges(&min, &max);
    GrowJudging(fixed_jumps);

    for (p = fixed_jumps; p < num_jumps; ++p)
    {
        LogJump(p);
    }

    if (num_jumps > fixed_jumps)
    {
        K_Log(K_LOG_MAP, K_LOG_INFO, "%d new jumps off %d edges, %.1f ms",
              num_jumps - fixed_jumps, edges,
              (SDL_GetPerformanceCounter() - begin) * 1000.0 / SDL_GetPerformanceFrequency());
    }

    return num_jumps - fixed_jumps;
}

int K_Jump_Count(void)
{
    return num_jumps;
}

const k_jump_t *K_Jump_Get(int jump)
{
    return &jumps[jump];
}

int K_Jump_Possible(int jump)
{
    const k_jump_t *j = &jumps[jump];
    run_t run;

    if (judged[jump] == leveltime + 1)
    {
        return possible[jump];
    }

    judged[jump] = leveltime + 1;
    Run(j->start, j->dir, THRUST, &run);
    possible[jump] = Lands(&run, j->land_sector);

    if (possible[jump])
    {
        Run(j->start, j->dir, THRUST * MARGIN_THRUST, &run);
        possible[jump] = Lands(&run, j->land_sector);
    }

    return possible[jump];
}
