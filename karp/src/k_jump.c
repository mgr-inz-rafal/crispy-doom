//
// DESCRIPTION:
//	Karp jumps (k_jump.h). The run is the engine's movement, replayed on
//	Karp's own data so nothing in the game is touched (plan.md, 2.4):
//	P_MovePlayer's thrust, P_XYMovement's moves in halves and friction,
//	P_ZMovement's gravity, P_TryMove's checks, with P_CheckPosition's floor
//	and ceiling under the box from the lines it straddles, and P_SlideMove
//	along a wall that stops a move.
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

#include "m_misc.h"

#include "k_graph.h"
#include "k_jump.h"
#include "k_log.h"
#include "k_specials.h"

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
// and at these angles to either side, in degrees. The steep ones reach a
// landing round a corner: Doom II MAP09's out of a door, along its wall, onto
// a ledge 48 degrees off to the side.
#define SAMPLE_SPACING 48.0

static const double ray_angles[] = { 0.0, 25.0, -25.0, 45.0, -45.0 };

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

// P_SlideMove stops this much of the move short of the wall.
#define SLIDE_FUDGE (0x800 / 65536.0)

#define MAX_LINES 128
#define MAX_BLOCKS 32

// Only this many jumps are logged one by one (and as many of those found
// as if a sector had moved).
#define MAX_REPORTS 12

// Searching as if a sector had moved: the edges this near it, and up to
// MAX_TARGETS places its lines send it.
#define AS_IF_REACH 512.0
#define MAX_TARGETS 4

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
    int door;               // the first door it took as open that is closed,
                            // -1 if none
    boolean as_if_used;     // it went over the sector searched as if moved
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

// Searching as if a sector were where a line sends it (K_Jump_Find): its
// floor and ceiling then, and every door open.
static int as_if = -1;
static double as_if_floor, as_if_ceiling;
static unsigned char *door_like;    // per sector: a manual door, or one a
                                    // line opens
static int assumed_door;            // the first door taken as open that is
                                    // closed, -1 if none (CheckPosition)
static boolean as_if_used;          // the box was over that sector

static double Height(fixed_t height)
{
    return height / (double) FRACUNIT;
}

static double Floor(const sector_t *sector)
{
    if (sector - sectors == as_if)
    {
        as_if_used = true;
        return as_if_floor;
    }

    return Height(sector->floorheight);
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
    return Floor(&sectors[CellSector(cell)]);
}

// A sector's ceiling for the run: a door already opening at its top; a
// manual door open while doors_open, and any door while searching as if a
// sector had moved.
static double Ceiling(const sector_t *sector)
{
    int index = (int) (sector - sectors);
    double ceiling = K_Graph_OpeningCeiling(index);
    double open;

    if (index == as_if)
    {
        as_if_used = true;
        return as_if_ceiling;
    }

    if (!(doors_open && K_Graph_IsDoor(index)) && !(as_if >= 0 && door_like[index]))
    {
        return ceiling;
    }

    open = SDL_max(ceiling, K_Graph_DoorTop(index));

    if (as_if >= 0 && open > ceiling + 0.5 && assumed_door < 0)
    {
        assumed_door = index;
    }

    return open;
}

// The cell a run starts or lands in. Searching as if a sector had moved,
// the floors the map knows are not those of the search, but the centre is
// over the floor it stands on then.
static int StandingCell(k_vec2_t at, double floor)
{
    return as_if >= 0 ? K_Map_CellAt(at.x, at.y) : K_Map_StandingCell(at.x, at.y, floor);
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

    *floorz = Floor(sector);
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
        *floorz = SDL_max(*floorz, SDL_max(Floor(line->frontsector), Floor(line->backsector)));
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

// Whether a line stops the player sliding along (PTR_SlideTraverse): a wall
// it is in front of, or a two-sided line it cannot get through. Impassable
// lines do not count, as in the engine.
static boolean SlideBlocks(const body_t *body, int index)
{
    const k_line_t *kline = &K_Map_Get()->lines[index];
    const line_t *line = &lines[index];
    double top, bottom;

    if (line->backsector == NULL)
    {
        // P_PointOnLineSide: in front, not behind it.
        return (kline->v2.x - kline->v1.x) * (body->y - kline->v1.y)
             < (kline->v2.y - kline->v1.y) * (body->x - kline->v1.x);
    }

    top = SDL_min(Ceiling(line->frontsector), Ceiling(line->backsector));
    bottom = SDL_max(Floor(line->frontsector), Floor(line->backsector));

    return top - bottom < PLAYER_HEIGHT || top - body->z < PLAYER_HEIGHT
        || bottom - body->z > MAX_STEP;
}

// One of P_SlideMove's traces, from a corner of the box along the momentum:
// the nearest line across it that stops the player, as a fraction of the
// momentum, if nearer than *best.
static void SlideTrace(const body_t *body, double x0, double y0, double *best, int *best_line)
{
    const k_map_t *map = K_Map_Get();
    double dx = body->momx, dy = body->momy;
    k_vec2_t min = { SDL_min(x0, x0 + dx), SDL_min(y0, y0 + dy) };
    k_vec2_t max = { SDL_max(x0, x0 + dx), SDL_max(y0, y0 + dy) };
    int found[MAX_LINES];
    int count = K_Graph_LinesNear(min, max, found, MAX_LINES);
    int i;

    for (i = 0; i < count; ++i)
    {
        const k_line_t *line = &map->lines[found[i]];
        double lx = line->v2.x - line->v1.x, ly = line->v2.y - line->v1.y;
        double side1 = dx * (line->v1.y - y0) - dy * (line->v1.x - x0);
        double side2 = dx * (line->v2.y - y0) - dy * (line->v2.x - x0);
        double denom = dx * ly - dy * lx;
        double frac;

        // Its ends on either side of the trace (PIT_AddLineIntercepts).
        if ((side1 > 0.0) == (side2 > 0.0) || fabs(denom) < 1e-12)
        {
            continue;
        }

        frac = ((y0 - line->v1.y) * lx - (x0 - line->v1.x) * ly) / denom;

        if (frac < 0.0 || frac > 1.0 || frac >= *best || !SlideBlocks(body, found[i]))
        {
            continue;
        }

        *best = frac;
        *best_line = found[i];
    }
}

// P_SlideMove: a move the player could not make slides it along the wall it
// ran into instead, and sets its momentum along it. The box goes round a
// wall's corner this way: Doom II MAP09's floor touches the next one only at
// a corner of a closed door, which the player slides along to get across.
static void SlideMove(body_t *body)
{
    int hitcount;

    for (hitcount = 1; hitcount < 3; ++hitcount)
    {
        double rx = body->momx > 0.0 ? PLAYER_RADIUS : -PLAYER_RADIUS;
        double ry = body->momy > 0.0 ? PLAYER_RADIUS : -PLAYER_RADIUS;
        double best = 1.0 + 1.0 / FRACUNIT;
        int line = -1;
        double move_x, move_y, lx, ly, along;

        // From the leading corners.
        SlideTrace(body, body->x + rx, body->y + ry, &best, &line);
        SlideTrace(body, body->x - rx, body->y + ry, &best, &line);
        SlideTrace(body, body->x + rx, body->y - ry, &best, &line);

        if (line < 0)
        {
            break;
        }

        // Up to the wall, a little short of it.
        best -= SLIDE_FUDGE;

        if (best > 0.0
         && !TryMove(body, body->x + body->momx * best, body->y + body->momy * best))
        {
            break;
        }

        // The rest of the move, along the wall (P_HitSlideLine).
        best = SDL_min(1.0 - (best + SLIDE_FUDGE), 1.0);

        if (best <= 0.0)
        {
            return;
        }

        lx = K_Map_Get()->lines[line].v2.x - K_Map_Get()->lines[line].v1.x;
        ly = K_Map_Get()->lines[line].v2.y - K_Map_Get()->lines[line].v1.y;
        along = (body->momx * lx + body->momy * ly) * best / (lx * lx + ly * ly);
        move_x = lx * along;
        move_y = ly * along;
        body->momx = move_x;
        body->momy = move_y;

        if (TryMove(body, body->x + move_x, body->y + move_y))
        {
            return;
        }
    }

    // No wall found, or the slide failed too: one axis, then the other.
    if (!TryMove(body, body->x, body->y + body->momy))
    {
        TryMove(body, body->x + body->momx, body->y);
    }
}

// One tic of the player's movement: the thrust (on the ground only), the
// move in halves while more than half of MAX_MOVE (only a positive move is
// checked, as in the engine), sliding along what stops it, friction on the
// ground, gravity. False if it could not move at all.
static boolean Tic(body_t *body, double thrust_x, double thrust_y)
{
    double from_x = body->x, from_y = body->y;
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
            SlideMove(body);
        }
    } while (xmove != 0.0 || ymove != 0.0);

    if (fabs(body->x - from_x) < 1e-6 && fabs(body->y - from_y) < 1e-6)
    {
        return false;
    }

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
    boolean left_ground = false;    // the run-up is over
    int tic;

    memset(run, 0, sizeof(*run));
    run->land_sector = -1;
    run->door = -1;
    run->edge = start;
    doors_open = true;
    assumed_door = -1;
    as_if_used = false;

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

        // Still on the run-up: on the ground all along, on the floor it
        // started on or a step it went up on the way.
        doors_open = !left_ground && body.z <= body.floorz && body.floorz > takeoff - 0.01
                  && body.floorz < takeoff + MAX_STEP + 0.01;

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
        left_ground |= body.z > body.floorz;

        under = SectorAt(body.x, body.y);
        lowest = SDL_min(lowest, Floor(under));

        // On the ground, its centre over the floor it stands on: landed if
        // that is too high to climb to from the lowest floor passed over,
        // and it stops there; if it cannot stop, it runs on (a pillar's top
        // on the way). Fell if it is down among them. Over an edge, the box
        // still stands on the floor it is leaving.
        if (body.z <= body.floorz && fabs(Floor(under) - body.floorz) < 0.01)
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
                    run->door = assumed_door;
                    run->as_if_used = as_if_used;
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

// The jump from a run-up's start along dir: kept if it lands with the
// margin, and is the shortest so far between its cells (one already in the
// graph stays as it is). If the margin fails to one side only, the start is
// moved MARGIN_SIDE the other way and tried once more: an opening a little
// wider than the player is only taken near its middle, which samples along
// an edge can miss (Doom II MAP14: the window to the red key, 64 wide,
// whose edge is two portals sampled 16 off its middle).
static void TryStart(k_vec2_t start, k_vec2_t dir, boolean recentred)
{
    k_vec2_t side = { -dir.y, dir.x };
    double from_floor, ceilingz;
    run_t run, check;
    int from_cell, to_cell, i, k, fails = 0, failed_side = 0;
    k_jump_t *jump;

    Run(start, dir, THRUST, &run);

    // Searching as if a sector had moved, only the jumps that go over it.
    if (run.result != RUN_LANDED || (as_if >= 0 && !run.as_if_used))
    {
        return;
    }

    // The floor the run-up starts on: the takeoff floor's, or a step below
    // it.
    doors_open = true;
    CheckPosition(start.x, start.y, &from_floor, &ceilingz);
    doors_open = false;
    from_cell = StandingCell(start, from_floor);
    to_cell = StandingCell(run.land, run.land_floor);

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
            ++fails;
            failed_side = k;
        }
    }

    if (fails > 0)
    {
        if (fails == 1 && !recentred)
        {
            k_vec2_t moved = { start.x - side.x * failed_side * MARGIN_SIDE,
                               start.y - side.y * failed_side * MARGIN_SIDE };

            TryStart(moved, dir, true);
        }

        return;
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
    jump->as_if_sector = as_if;
    jump->as_if_floor = as_if_floor;
    jump->as_if_ceiling = as_if_ceiling;
    jump->door = run.door;
}

// The point `back` behind `at`, against dir.
static k_vec2_t Back(k_vec2_t at, k_vec2_t dir, double back)
{
    k_vec2_t point = { at.x - dir.x * back, at.y - dir.y * back };

    return point;
}

// The jumps off the edge at `at`, along dir, from the floor and sector it
// drops from: with the longest run-up there is room for, and with a short
// one, which a gap that needs little speed takes without first backing up
// (Doom II MAP08: pillars that sink once stepped on).
static void TryJump(k_vec2_t at, k_vec2_t dir, double from_floor, int from_sector)
{
    double back = 0.0, last = from_floor;
    double floorz, ceilingz;

    // The run-up, behind the edge on the takeoff floor (doors open, as in a
    // run), or on floors below it that the player only climbs from, up to a
    // step below: going up a step costs no speed (Doom II MAP14: the jump
    // over a channel to the red key starts from a step 16 deep, run up to
    // from the corridor before it). Its centre never crosses a door's side:
    // nav would not know to open the door on the way.
    doors_open = true;

    while (back + RUNUP_STEP <= RUNUP_MAX)
    {
        double x = at.x - dir.x * (back + RUNUP_STEP);
        double y = at.y - dir.y * (back + RUNUP_STEP);
        sector_t *sector = SectorAt(x, y);
        int index = (int) (sector - sectors);

        if (!CheckPosition(x, y, &floorz, &ceilingz)
         || floorz > last + 0.01 || floorz < from_floor - MAX_STEP - 0.01
         || ceilingz - floorz < PLAYER_HEIGHT
         || Floor(sector) < from_floor - MAX_STEP - 0.01
         || (index != from_sector && (K_Graph_IsDoor(index) || K_Graph_IsDoor(from_sector))))
        {
            break;
        }

        last = floorz;
        back += RUNUP_STEP;
    }

    doors_open = false;

    TryStart(Back(at, dir, back), dir, false);

    if (back > RUNUP_SHORT)
    {
        TryStart(Back(at, dir, RUNUP_SHORT), dir, false);
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

                for (a = 0; a < (int) (sizeof(ray_angles) / sizeof(*ray_angles)); ++a)
                {
                    double angle = ray_angles[a] * K_PI / 180.0;
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
    char as_if_text[64] = "";

    if (jump->as_if_sector >= 0)
    {
        M_snprintf(as_if_text, sizeof(as_if_text), " - once sector %d moves (floor %.0f, "
                   "ceiling %.0f)", jump->as_if_sector, jump->as_if_floor, jump->as_if_ceiling);
    }

    K_Log(K_LOG_MAP, K_LOG_INFO, "jump from (%.0f,%.0f) cell %d to (%.0f,%.0f) cell %d: "
          "%.0f units off the edge at %.1f units a tic%s", jump->start.x, jump->start.y,
          jump->from_cell, jump->land.x, jump->land.y, jump->to_cell, jump->flight,
          jump->speed, as_if_text);
}

// The box round a sector's lines, grown by AS_IF_REACH; false if it has none.
static boolean SectorBox(int sector, k_vec2_t *min, k_vec2_t *max)
{
    const k_map_t *map = K_Map_Get();
    int i;

    min->x = min->y = HUGE_VAL;
    max->x = max->y = -HUGE_VAL;

    for (i = 0; i < map->num_lines; ++i)
    {
        const k_line_t *line = &map->lines[i];

        if (line->front_sector == sector || line->back_sector == sector)
        {
            min->x = SDL_min(min->x, SDL_min(line->v1.x, line->v2.x));
            min->y = SDL_min(min->y, SDL_min(line->v1.y, line->v2.y));
            max->x = SDL_max(max->x, SDL_max(line->v1.x, line->v2.x));
            max->y = SDL_max(max->y, SDL_max(line->v1.y, line->v2.y));
        }
    }

    min->x -= AS_IF_REACH;
    min->y -= AS_IF_REACH;
    max->x += AS_IF_REACH;
    max->y += AS_IF_REACH;
    return min->x < max->x;
}

// The doors every search as if a sector had moved takes as open: manual
// ones, and those a line opens.
static void FindDoors(void)
{
    int s, k;

    free(door_like);
    door_like = calloc(SDL_max(numsectors, 1), 1);

    for (s = 0; s < numsectors; ++s)
    {
        door_like[s] = K_Graph_IsDoor(s);

        for (k = 0; k < K_Specials_TriggerCount(s); ++k)
        {
            int special = K_Specials_LineSpecial(K_Specials_TriggerLine(s, k));

            if (K_Specials_MoverMoves(special) & K_MOVE_CEILING_UP)
            {
                door_like[s] = 1;
            }
        }
    }
}

// The jumps around each sector a line moves, as if it were where the line
// sends it (each place once), with every door open. Jumps found already
// stay as they are: one between the same cells that works as things are is
// better. Returns how many sectors were searched as moved.
static int SearchAsIf(void)
{
    int searched = 0;
    int s, k, j;

    FindDoors();

    for (s = 0; s < numsectors; ++s)
    {
        double targets[MAX_TARGETS][2];
        int num_targets = 0;
        k_vec2_t min, max;

        if (!SectorBox(s, &min, &max))
        {
            continue;
        }

        for (k = 0; k < K_Specials_TriggerCount(s) && num_targets < MAX_TARGETS; ++k)
        {
            double floor, ceiling;

            if (!K_Specials_MoverTarget(K_Specials_TriggerLine(s, k), s, &floor, &ceiling)
             || (fabs(floor - Height(sectors[s].floorheight)) < 0.5
              && fabs(ceiling - Height(sectors[s].ceilingheight)) < 0.5))
            {
                continue;
            }

            for (j = 0; j < num_targets; ++j)
            {
                if (fabs(targets[j][0] - floor) < 0.5 && fabs(targets[j][1] - ceiling) < 0.5)
                {
                    break;
                }
            }

            if (j < num_targets)
            {
                continue;
            }

            targets[num_targets][0] = floor;
            targets[num_targets][1] = ceiling;
            ++num_targets;

            as_if = s;
            as_if_floor = floor;
            as_if_ceiling = ceiling;
            fixed_jumps = num_jumps;
            SearchEdges(&min, &max);
            as_if = -1;
            ++searched;
        }
    }

    return searched;
}

void K_Jump_Find(void)
{
    const k_map_t *map = K_Map_Get();
    Uint64 begin = SDL_GetPerformanceCounter();
    int edges, found, moved, p;

    num_jumps = 0;
    fixed_jumps = 0;
    edges = SearchEdges(NULL, NULL);
    found = num_jumps;
    moved = SearchAsIf();

    free(judged);
    free(possible);
    judged = NULL;
    possible = NULL;
    GrowJudging(0);

    for (p = 0; p < num_jumps && p < MAX_REPORTS; ++p)
    {
        LogJump(p);
    }

    for (p = SDL_max(found, MAX_REPORTS); p < num_jumps && p < found + MAX_REPORTS; ++p)
    {
        LogJump(p);
    }

    K_Log(K_LOG_MAP, K_LOG_INFO, "%s: %d jumps off %d edges, and %d more once one of %d "
          "sectors has moved, %.1f ms", map->name, found, edges, num_jumps - found, moved,
          (SDL_GetPerformanceCounter() - begin) * 1000.0 / SDL_GetPerformanceFrequency());
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
