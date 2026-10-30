//
// DESCRIPTION:
//	Karp navigation: follows the route to the goal.
//
//	Think, once per game tic (K_Nav_Tick): which route cell the player is
//	in and which waypoint to drive to next, doors and lifts on the way,
//	arrival, and replanning when the player leaves the route or gets
//	stuck.
//
//	Act, once per command (K_Nav_BuildCommand): a command is built a tic
//	or two before it runs, so the controller first predicts where the
//	player will be by then, replaying the commands still waiting to run.
//	It then picks the velocity it wants (full speed, slowing down for
//	corners, doors and the goal) and thrusts the difference between that
//	and the current momentum, split into forward and strafe. Movement
//	therefore does not depend on which way the player faces, and pushing
//	against the momentum brakes.
//
//	Doors and lifts: at a crossing that needs one, the player stops at a
//	spot in front of the line, turns to face it and presses use - only
//	when the engine's use trace (64 units straight ahead) would hit it,
//	and never on a door already opening, since that closes it. Then it
//	waits for as long as the door or lift keeps moving. A lift that comes
//	down is boarded like any open floor; on it, the crossing off it waits
//	for the ride up.
//
//	Gun switches are worked the same way, but shot: with a hitscan weapon,
//	brought up first if need be, and only once it is ready to fire.
//
//	Teleporters: the player runs across the line, and the route goes on
//	from the destination. No slowing down: a teleport stops the player
//	anyway, and freezes it for a moment, which the prediction knows.
//
//	Jumps (k_jump.h): from the run-up's start the player runs straight at
//	full speed along the line to the touchdown, held on the line while on
//	the ground; in the air nothing steers. The cells it crosses on the way
//	are off the route; down in the gap, it missed, and replans.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "d_loop.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"
#include "r_state.h"

#include "k_jump.h"
#include "k_log.h"
#include "k_nav.h"
#include "k_specials.h"

#define K_PI 3.14159265358979323846

// Player physics (p_user.c, p_mobj.c): a move of 1 thrusts 2048/65536
// map units per tic; friction applies on the ground.
#define THRUST_SCALE (2048.0 / 65536.0)
#define FRICTION (0xe800 / 65536.0)
#define STOPSPEED (0x1000 / 65536.0)
#define MAXMOVE 30.0

// The largest moves a running human makes; crispy reports faster forward
// moves as turbo.
#define MAX_FORWARD 50
#define MAX_SIDE 40

#define ANGLETURN_PER_RADIAN (65536.0 / (2.0 * K_PI))

#define ARRIVE_RADIUS 16.0
#define ARRIVE_SPEED 1.0

// Stuck: moved less than STUCK_DISTANCE in STUCK_TICS tics. After
// MAX_STUCK replans in a row the goal fails; the count starts over once
// the player gets UNSTUCK_DISTANCE away from where it last got stuck.
#define STUCK_TICS 35
#define STUCK_DISTANCE 8.0
#define MAX_STUCK 3
#define UNSTUCK_DISTANCE 64.0

// Corners slow the player down: full speed through a straight line, down
// to this fraction of it through a right angle or sharper.
#define MIN_CORNER_SPEED 0.2

// How far along the route the speed profile looks for corners.
#define LOOKAHEAD 400.0

// Near an edge the player could fall over but not climb back (a bridge
// over a pit, a ledge), within CAREFUL_REACH of it, a waypoint where the
// route turns by more than CAREFUL_TURN degrees, or steps down, is taken
// at no more than CAREFUL_SPEED, less for a sharp corner. Turning at speed
// swings the player wide, and stepping down leaves it airborne for a few
// tics, with no friction and no control: at full speed it would still be
// turning as it left a step, and fly on straight off the edge (Doom II
// MAP17: a zig-zag bridge over a pit, 46 wide, each zig a step of 16 down
// from the last). Straight on, past an edge, there is no need to slow.
#define CAREFUL_REACH 40.0
#define CAREFUL_SPEED 6.0
#define CAREFUL_TURN 20.0
#define CAREFUL_ADVANCE 8.0

// A fall carries on at the speed the player left the edge at, with no
// friction in the air, for as long as it falls: about sqrt(2h) tics for h
// units. Off a careful step down, where the fall could carry the player
// off the floor the route lands on (at CAREFUL_SPEED, toward the next
// waypoint, over a cell neither further along the route nor of the sector
// it lands in, before a wall stops it), it goes no faster than drifts
// DROP_DRIFT in that time, from the waypoint until it is off the ledge (its
// box clears the edge 16 units past the waypoint). Doom II MAP29: off a
// ledge into a rising blood channel 100 deep and 476 below, left at 8 units
// a tic, the player flew 240 units, out of the channel into the blood pit
// beside it, level with it then, and had to go the long way round. In the
// open, where it lands anyway, drops are not slowed (slowing every drop
// over 48 cost the regression maps 25 s).
#define DROP_DRIFT 64.0
#define DRIFT_STEP 8.0

// Slowing down for a careful waypoint, only this fraction of the usual
// braking is counted on: the controller lags a tic or more behind, and on
// an 8-unit ledge (Doom II MAP24) the full figure got the player to a right
// angle at 7 units a tic instead of 1.
#define CAREFUL_BRAKING 0.5

// Commands sent, by tic, for prediction.
#define SENT_SIZE 16

// Doors and lifts. The player stands ACTION_DISTANCE in front of the line
// (its radius is 16) and presses use when the trace would hit the line
// within USE_REACH (the engine's USERANGE is 64). It turns to face it at
// up to ACTION_TURN degrees a tic, and holds the spot while waiting.
#define ACTION_DISTANCE 24.0
#define USE_REACH 60.0

// How far a hitscan attack reaches (p_local.h): MELEERANGE for the fist and
// the chainsaw, MISSILERANGE for bullets; a little less, to be sure.
#define MELEE_REACH 60.0
#define SHOT_REACH 2000.0
#define ATTACK_HOLD 5
#define ACTION_TURN 45.0
#define SPOT_RADIUS 6.0
#define HOLD_GAIN 0.3
#define HOLD_SPEED 3.0

// Giving up: not at the spot after APPROACH_TICS (but within reach of the
// line, good enough), never got to press within AIM_TICS, a press that
// moved nothing within PRESS_TICS, MAX_PRESSES presses that did nothing,
// nothing moving for STALL_TICS while waiting (a lift waits 3 seconds at
// the bottom, so more than that).
#define APPROACH_TICS 70
#define AIM_TICS 105
#define PRESS_TICS 15
#define MAX_PRESSES 3
#define STALL_TICS 175

// A switch whose sector is still on its way (a door closing by itself) is
// waited at, for up to BUSY_TICS, and pressed once the sector stops.
#define BUSY_TICS (15 * TICRATE)

// Further than the player can move in a tic (MAXMOVE on each axis): it
// teleported.
#define TELEPORT_JUMP 64.0

// The highest step the player climbs (P_TryMove).
#define MAX_CLIMB 24.0

// Waiting dry: a crossing that opens by itself, in front of which the
// player would stand on a floor that hurts, is waited for where the route
// steps onto that floor, DRY_BACK short of it. The player goes on once the
// wait is down to the way there and DRY_MARGIN tics more, the way timed at
// DRY_SPEED (slower than running, from a standstill) with each fall's time
// on top, so that it gets there early rather than late. It waits dry only
// to save DRY_SAVING tics or more on that floor (a hit comes every 32).
#define DRY_BACK 24.0
#define DRY_MARGIN 35.0
#define DRY_SPEED 10.0
#define DRY_SAVING 32.0

// Sliding along walls: within AVOID_DISTANCE of anything that stops the
// player (its radius is 16), the part of the velocity toward it is
// dropped; touching it, the player eases away at AVOID_PUSH.
#define AVOID_DISTANCE 20.0
#define AVOID_TOUCH 17.0
#define AVOID_PUSH 1.0
#define MAX_AVOID 16

// A goal spot is clear of solid things when the player's box (radius 16)
// is a unit off every one; past a walk-over line, also of walls.
#define STAND_CLEARANCE 17.0

// Running a jump, the player is eased back onto its line at JUMP_LINE_GAIN
// of how far off it is, a tic, up to JUMP_LINE_SPEED.
#define JUMP_LINE_GAIN 0.5
#define JUMP_LINE_SPEED 4.0

// Going round solid things: the engine stops a move that would overlap
// one, a whole step before the boxes touch, so this starts further out
// than for walls.
#define AVOID_BLOCK_DISTANCE 32.0
#define MAX_AVOID_BLOCKS 16

typedef struct
{
    double x, y;
    double momx, momy;
    double angle;
    boolean onground;
    int frozen;
    double floor;           // the floor it stands on (now, not predicted)
} body_t;

typedef struct
{
    int tic;                // -1 when empty
    k_command_t command;
    k_vec2_t predicted;     // where the player should be after the tic
} sent_t;

typedef enum
{
    PHASE_APPROACH,         // going to the spot in front of the line
    PHASE_AIM,              // standing there, facing the line, pressing use
    PHASE_WAIT,             // waiting for the door or lift
} phase_t;

static const char *phase_names[] = { "walking up", "pressing", "waiting" };

typedef struct
{
    k_navaction_t kind;
    phase_t phase;
    int route_cell;         // the crossing is the route's portals[route_cell]
    int sector;             // the door or lift
    int line;               // the line to use, -1 if none
    k_vec2_t a, b;          // the portal
    k_vec2_t spot;          // where to stand
    double face;            // the angle that looks at the portal
    int tics;               // in this phase
    int start_tics;         // status.tics when it started
    int still;              // tics the door or lift has not moved
    double height;          // of its moving plane, last tic
    k_mover_t mover;        // what moved it, last tic
    int presses;
    int press_tic;          // the tic of the last press
    boolean jump_ride;      // a ride that ends in a jump: facing its way
} action_t;

static k_path_t path;
static unsigned char *careful;  // per route point: near an edge (CAREFUL_REACH)
static int careful_capacity;
static boolean have_goal;
static k_vec2_t goal;
static k_navstatus_t status;

// K_GOAL_CROSS_LINE, when the route to the point past the line went round
// its end: first to the point before it, then across to cross_to.
static boolean cross_first;
static k_vec2_t cross_to;
static k_navtuning_t tuning = { 16.7, 1.5, 24.0, 11.25 };
static action_t action;

// Waiting dry (DRY_BACK): the waypoint where the route steps onto the floor
// that hurts, which the target stops at, -1 when not; how far short of it
// the player stands, and since when (status.tics).
static int dry_point = -1;
static double dry_back;
static int dry_since;

static sent_t sent[SENT_SIZE];

// The way combat wants the player to face, for the command being built, or
// NULL (K_Nav_BuildCommand).
static const double *aim;

// A fight holds the route (K_Nav_Pause).
static boolean paused;

// Positions over the last STUCK_TICS tics.
static k_vec2_t history[STUCK_TICS];
static int history_count;
static int history_next;
static k_vec2_t stuck_at;
static k_vec2_t last_at;    // after the previous tic
static double last_floor;   // the floor stood on then

// How far into a jump the player is. Landed, it brakes to below
// SETTLE_SPEED (for at most SETTLE_TICS) before going on, as the jump was
// checked to stop on its landing (k_jump.c): it can be a narrow ledge.
typedef enum
{
    JUMP_NONE,
    JUMP_RUNUP,
    JUMP_AIR,
    JUMP_SETTLE,
} jumpphase_t;

#define SETTLE_SPEED 2.0
#define SETTLE_TICS 35

// Touching down within LANDING_REACH of a jump's touchdown, on its floor,
// is the landing; anywhere else the run goes on (a pillar's top on the
// way, Doom II MAP06).
#define LANDING_REACH 96.0

static jumpphase_t jump_phase;
static int settle_tics;
static double runup_floor;  // the floor the run-up was last on the ground on

static double Distance(k_vec2_t a, k_vec2_t b)
{
    return sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

static int Round(double value)
{
    return (int) floor(value + 0.5);
}

//
// Prediction
//

// One tic of player movement, as P_PlayerThink, P_MovePlayer and
// P_XYMovement do it, without walls. Just after a teleport, the player
// neither turns nor thrusts.
static void Step(body_t *body, const k_command_t *command)
{
    if (body->frozen > 0)
    {
        --body->frozen;
    }
    else
    {
        body->angle += command->turn / ANGLETURN_PER_RADIAN;

        if (body->onground)
        {
            double forward = command->forward * THRUST_SCALE;
            double side = command->side * THRUST_SCALE;

            body->momx += forward * cos(body->angle) + side * sin(body->angle);
            body->momy += forward * sin(body->angle) - side * cos(body->angle);
        }
    }

    body->momx = SDL_max(-MAXMOVE, SDL_min(body->momx, MAXMOVE));
    body->momy = SDL_max(-MAXMOVE, SDL_min(body->momy, MAXMOVE));
    body->x += body->momx;
    body->y += body->momy;

    if (body->onground)
    {
        if (fabs(body->momx) < STOPSPEED && fabs(body->momy) < STOPSPEED
         && command->forward == 0 && command->side == 0)
        {
            body->momx = body->momy = 0.0;
        }
        else
        {
            body->momx *= FRICTION;
            body->momy *= FRICTION;
        }
    }
}

// The player as it will be when tic `tic` starts: the current state, with
// the commands already sent for the tics before it replayed.
static boolean Predict(int tic, body_t *body)
{
    static const k_command_t idle;
    k_pose_t pose;
    int t;

    if (!K_Map_GetPlayer(&pose))
    {
        return false;
    }

    body->x = pose.x;
    body->y = pose.y;
    body->momx = pose.momx;
    body->momy = pose.momy;
    body->angle = pose.angle;
    body->onground = pose.onground != 0;
    body->frozen = pose.frozen;
    body->floor = pose.floor;

    for (t = SDL_max(gametic, tic - SENT_SIZE); t < tic; ++t)
    {
        const sent_t *entry = &sent[t % SENT_SIZE];

        Step(body, entry->tic == t ? &entry->command : &idle);
    }

    return true;
}

static void Record(int tic, const k_command_t *command, const body_t *before)
{
    sent_t *entry = &sent[tic % SENT_SIZE];
    body_t after = *before;

    Step(&after, command);
    entry->tic = tic;
    entry->command = *command;
    entry->predicted.x = after.x;
    entry->predicted.y = after.y;
}

// Called right after tic `gametic` ran.
static void CheckPrediction(k_vec2_t at)
{
    const sent_t *entry = &sent[gametic % SENT_SIZE];

    if (entry->tic == gametic)
    {
        status.prediction_error += (Distance(at, entry->predicted)
                                  - status.prediction_error) / TICRATE;
    }
}

//
// The route
//

// The position of a cell in the route, or -1. A teleporter can bring the
// route back to a cell it went through before: the visit from the
// player's route cell on wins.
static int RouteIndex(int cell)
{
    int from = SDL_max(status.route_cell, 0);
    int i;

    for (i = from; i < path.num_cells; ++i)
    {
        if (path.cells[i] == cell)
        {
            return i;
        }
    }

    for (i = 0; i < SDL_min(from, path.num_cells); ++i)
    {
        if (path.cells[i] == cell)
        {
            return i;
        }
    }

    return -1;
}

// The first waypoint reached by a leg inside route cell r.
static int FirstPointIn(int r)
{
    int j;

    for (j = 1; j < path.num_points - 1; ++j)
    {
        if (path.leg_cells[j - 1] >= r)
        {
            return j;
        }
    }

    return path.num_points - 1;
}

// The waypoint at the way out of route cell r: the middle of portals[r].
static int ExitPoint(int r)
{
    return FirstPointIn(r + 1) - 1;
}

static k_crossing_t CrossingOut(int r)
{
    return K_Graph_CanCross(path.portals[r], path.cells[r]);
}

// Whether the route leaves cell r by teleporter, as planned: the leg
// after the point past its line is the teleport.
static boolean TeleportOut(int r)
{
    return path.legs[ExitPoint(r)] == K_LEG_TELEPORT;
}

// Whether the target is the end of a jump's leg: the player is running it.
static boolean OnJump(int target)
{
    return target > 0 && target < path.num_points && path.legs[target - 1] == K_LEG_JUMP;
}

// The first crossing from the player's route cell on that is not open yet
// (a door, a lift, a teleporter...), or -1. Not just the next one: the
// player can stand just off the route, in a cell beside it, while the
// route cell it last was in leads on openly. A jump that works is run like
// an open crossing.
static int NextClosedCrossing(void)
{
    int r;

    for (r = SDL_max(status.route_cell, 0); r < path.num_cells - 1; ++r)
    {
        k_crossing_t crossing = CrossingOut(r);

        if ((crossing != K_CROSS_OPEN && crossing != K_CROSS_JUMP) || TeleportOut(r))
        {
            return r;
        }
    }

    return -1;
}

// The furthest waypoint the target may move to: not past a crossing that
// is not open yet, nor where the player waits dry.
static int TargetLimit(void)
{
    int r = NextClosedCrossing();
    int limit = r >= 0 ? ExitPoint(r) : path.num_points - 1;

    return dry_point >= 0 ? SDL_min(limit, dry_point) : limit;
}

// Where the player waits dry: dry_back short of dry_point, back along the
// leg into it.
static k_vec2_t DrySpot(void)
{
    k_vec2_t from = path.points[dry_point - 1], to = path.points[dry_point];
    double length = Distance(from, to);
    k_vec2_t spot = to;

    if (length > 1e-6)
    {
        spot.x -= (to.x - from.x) / length * dry_back;
        spot.y -= (to.y - from.y) / length * dry_back;
    }

    return spot;
}

//
// Controller
//

static double CellFloor(int cell);

// Whether a fall of `tics` off route point k, left at CAREFUL_SPEED toward
// the next point, could carry the player off the floor the route lands on
// (DROP_DRIFT).
static boolean DriftsOff(int k, double tics)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t from = path.points[k], to = path.points[k + 1];
    double length = Distance(from, to);
    double reach = CAREFUL_SPEED * tics + 16.0;     // off the ledge 16 on
    int first = path.leg_cells[k];
    int sector = map->subsectors[map->cells[path.cells[first]].subsector].sector;
    double t;
    int i;

    for (t = 16.0; length > 1e-6 && t <= reach; t += DRIFT_STEP)
    {
        int cell = K_Map_CellAt(from.x + (to.x - from.x) / length * t,
                                from.y + (to.y - from.y) / length * t);

        // A wall stops it.
        if (cell < 0)
        {
            return false;
        }

        if (map->subsectors[map->cells[cell].subsector].sector == sector)
        {
            continue;
        }

        for (i = first; i < path.num_cells && path.cells[i] != cell; ++i)
        {
        }

        if (i == path.num_cells)
        {
            return true;
        }
    }

    return false;
}

// The top speed at a careful waypoint: CAREFUL_SPEED, or less where the
// route drops there and the fall could carry the player off the floor the
// route lands on, so that it drifts no more than DROP_DRIFT.
static double CarefulSpeed(int k)
{
    double fall = CellFloor(path.cells[path.leg_cells[k - 1]])
                - CellFloor(path.cells[path.leg_cells[k]]);
    double tics = sqrt(2.0 * SDL_max(fall, 0.0)) + 1.0;

    if (fall <= MAX_CLIMB || DROP_DRIFT / tics >= CAREFUL_SPEED || k + 1 >= path.num_points
     || !DriftsOff(k, tics))
    {
        return CAREFUL_SPEED;
    }

    return DROP_DRIFT / tics;
}

// The speed to take a waypoint at, from how sharply the route turns there,
// out of the top speed there (careful near an edge). 0 in front of a door
// or lift; full speed into a teleporter, which stops the player anyway,
// and out of one, where there is no corner, and at a jump's touchdown,
// reached in the air. A jump's run-up start is a corner like any other.
static double CornerSpeed(int k)
{
    k_vec2_t a = path.points[k - 1], b = path.points[k], c = path.points[k + 1];
    double in = Distance(a, b), out = Distance(b, c);
    double top = careful[k] ? SDL_min(CarefulSpeed(k), tuning.max_speed) : tuning.max_speed;
    double turn_cos = 1.0;
    int r = path.leg_cells[k - 1];
    k_crossing_t crossing;

    if (path.legs[k] == K_LEG_TELEPORT || path.legs[k - 1] != K_LEG_WALK)
    {
        return tuning.max_speed;
    }

    crossing = path.leg_cells[k] != r ? CrossingOut(r) : K_CROSS_OPEN;

    if (crossing != K_CROSS_OPEN && crossing != K_CROSS_JUMP)
    {
        return 0.0;
    }

    if (in > 1e-6 && out > 1e-6)
    {
        turn_cos = ((b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y))
                 / (in * out);
    }

    return top * SDL_max(MIN_CORNER_SPEED, SDL_min(turn_cos, 1.0));
}

// How near a waypoint the target moves on to the next: a careful one is
// driven right up to, at its corner speed, before turning; cutting it
// short would turn at the speed of the approach, and swing wide.
static double AdvanceRadius(int k)
{
    return careful[k] ? CAREFUL_ADVANCE : tuning.advance_radius;
}

// The fastest the player may go now and still slow down in time for the
// corners ahead and stop at the goal.
static double SpeedLimit(const body_t *body, int target)
{
    k_vec2_t at = { body->x, body->y };
    double limit = tuning.max_speed;
    double along = Distance(at, path.points[target]);
    int last = path.num_points - 1;
    int k;

    // Running a jump: full speed, whatever lies beyond.
    if (OnJump(target))
    {
        return tuning.max_speed;
    }

    for (k = target; k <= last && along < LOOKAHEAD; ++k)
    {
        // Waiting dry, the stop is short of the waypoint.
        double corner = k == last || k == dry_point ? 0.0 : CornerSpeed(k);
        double ahead = k == dry_point ? SDL_max(along - dry_back, 0.0) : along;
        double braking = careful[k] ? tuning.braking * CAREFUL_BRAKING : tuning.braking;

        limit = SDL_min(limit, sqrt(corner * corner + 2.0 * braking * ahead));

        // Past a stop, a teleporter or a jump's run-up start, nothing ahead
        // needs slowing for now.
        if (corner == 0.0 || path.legs[k] != K_LEG_WALK)
        {
            break;
        }

        if (k < last)
        {
            along += Distance(path.points[k], path.points[k + 1]);
        }
    }

    return limit;
}

// How far a thrust is beyond what the controls give (1 at the limit),
// facing `angle`.
static double ControlScale(double angle, double thrust_x, double thrust_y)
{
    double forward = (thrust_x * cos(angle) + thrust_y * sin(angle)) / THRUST_SCALE;
    double side = (thrust_x * sin(angle) - thrust_y * cos(angle)) / THRUST_SCALE;

    return SDL_max(fabs(forward) / MAX_FORWARD, fabs(side) / MAX_SIDE);
}

// Thrusts whatever turns the momentum into the desired velocity
// (desired_x, desired_y), and turns toward it - or toward the angle face,
// if given. With drift_first, when that is more than the controls give,
// the momentum across the desired way is cancelled first, and only what
// is left speeds the player up along it; else both are scaled alike.
static void Steer(const body_t *body, double desired_x, double desired_y,
                  const double *face, boolean drift_first, k_command_t *command)
{
    double max_turn = (face != NULL ? ACTION_TURN : tuning.max_turn) * K_PI / 180.0;
    double desired = sqrt(desired_x * desired_x + desired_y * desired_y);
    double turn = 0.0;
    double angle, thrust_x, thrust_y, forward, side, scale;

    // Just teleported: the command would do nothing.
    if (body->frozen > 0)
    {
        return;
    }

    // Facing is cosmetic while driving: strafing moves the player just as
    // well.
    if (face != NULL || desired_x * desired_x + desired_y * desired_y > 0.25)
    {
        turn = (face != NULL ? *face : atan2(desired_y, desired_x)) - body->angle;
        turn = atan2(sin(turn), cos(turn));
        turn = SDL_max(-max_turn, SDL_min(turn, max_turn));
    }

    command->turn = Round(turn * ANGLETURN_PER_RADIAN);

    // Demos recorded without -longtics store turns in steps of 256.
    if (lowres_turn)
    {
        command->turn = (int) floor((command->turn + 128) / 256.0) * 256;
    }

    if (!body->onground)
    {
        return;
    }

    // The turn applies before the thrust in the same tic.
    angle = body->angle + command->turn / ANGLETURN_PER_RADIAN;
    thrust_x = desired_x - body->momx;
    thrust_y = desired_y - body->momy;

    // Speeding up along the way waits for the drift across it; slowing
    // down along it does not.
    if (drift_first && desired > 1e-6 && ControlScale(angle, thrust_x, thrust_y) > 1.0)
    {
        double ux = desired_x / desired, uy = desired_y / desired;
        double along = thrust_x * ux + thrust_y * uy;
        double across_x = thrust_x - along * ux, across_y = thrust_y - along * uy;
        double low = 0.0, high = 1.0;
        int i;

        if (along > 0.0)
        {
            for (i = 0; i < 12; ++i)
            {
                double f = (low + high) * 0.5;

                if (ControlScale(angle, across_x + along * f * ux, across_y + along * f * uy)
                    > 1.0)
                {
                    high = f;
                }
                else
                {
                    low = f;
                }
            }

            thrust_x = across_x + along * low * ux;
            thrust_y = across_y + along * low * uy;
        }
    }

    forward = (thrust_x * cos(angle) + thrust_y * sin(angle)) / THRUST_SCALE;
    side = (thrust_x * sin(angle) - thrust_y * cos(angle)) / THRUST_SCALE;

    // More thrust than the controls give: scale both down, keeping the
    // direction.
    scale = SDL_max(1.0, SDL_max(fabs(forward) / MAX_FORWARD, fabs(side) / MAX_SIDE));
    command->forward = Round(forward / scale);
    command->side = Round(side / scale);
}

// Drives to `to` and stops there.
static void SteerToStop(const body_t *body, k_vec2_t to, const double *face,
                        k_command_t *command)
{
    k_vec2_t at = { body->x, body->y };
    double distance = Distance(at, to);
    double speed = SDL_min(tuning.max_speed,
                           SDL_min(sqrt(2.0 * tuning.braking * distance), distance));

    if (distance < 1e-6)
    {
        Steer(body, 0.0, 0.0, face, false, command);
        return;
    }

    Steer(body, (to.x - at.x) / distance * speed, (to.y - at.y) / distance * speed,
          face, false, command);
}

// Stays near `to`, without hurrying.
static void SteerToHold(const body_t *body, k_vec2_t to, const double *face,
                        k_command_t *command)
{
    double x = (to.x - body->x) * HOLD_GAIN;
    double y = (to.y - body->y) * HOLD_GAIN;
    double length = sqrt(x * x + y * y);

    if (length > HOLD_SPEED)
    {
        x *= HOLD_SPEED / length;
        y *= HOLD_SPEED / length;
    }

    Steer(body, x, y, face, false, command);
}

// The route keeps its legs inside cells, but not clear of the corners
// along them: a leg can pass so close to one that the player's box catches
// on it, pushing straight in, with nothing for the engine to slide it
// along (Doom II MAP03, leaving a switch alcove past a pillar). So near
// any line that stops the player, the part of the velocity toward it is
// dropped: the player slides along instead, and eases away when touching.
// Solid things are gone round instead (AvoidBlocks).
// Near is measured with the player's square box, and away is from the
// nearest point: a wall end caught by the side of the box turns the player
// round it (MAP16, a wall stub at the top of a staircase).
static void AvoidWalls(const body_t *body, double *vx, double *vy)
{
    k_nearline_t walls[MAX_AVOID];
    k_vec2_t at = { body->x, body->y };
    int count = K_Graph_ObstaclesNear(at, body->floor, AVOID_DISTANCE, walls, MAX_AVOID);
    int i;

    for (i = 0; i < count; ++i)
    {
        double into = *vx * walls[i].away.x + *vy * walls[i].away.y;

        if (into < 0.0)
        {
            *vx -= into * walls[i].away.x;
            *vy -= into * walls[i].away.y;
        }

        if (walls[i].distance < AVOID_TOUCH)
        {
            *vx += walls[i].away.x * AVOID_PUSH;
            *vy += walls[i].away.y * AVOID_PUSH;
        }
    }
}

// The engine slides the player along walls but not along solid things
// (P_SlideMove only traces lines): against one it stops dead. So the
// player goes round them as round a pillar: near one, the part of the
// velocity toward it is turned along its side, the way the target lies, or
// round the side the player is on if the target is straight past it (Doom
// II MAP27: a leg grazing a skull pile's square stopped the player head
// on, with nothing left to slide along).
static void AvoidBlocks(const body_t *body, k_vec2_t target, double *vx, double *vy)
{
    const k_map_t *map = K_Map_Get();
    k_vec2_t at = { body->x, body->y };
    k_vec2_t min = { at.x - AVOID_BLOCK_DISTANCE, at.y - AVOID_BLOCK_DISTANCE };
    k_vec2_t max = { at.x + AVOID_BLOCK_DISTANCE, at.y + AVOID_BLOCK_DISTANCE };
    int blocks[MAX_AVOID_BLOCKS];
    int count = K_Map_BlocksNear(min, max, blocks, MAX_AVOID_BLOCKS);
    int i;

    for (i = 0; i < count; ++i)
    {
        const k_block_t *block = &map->blocks[blocks[i]];
        double dx = at.x - SDL_max(block->at.x - block->radius,
                                   SDL_min(block->at.x + block->radius, at.x));
        double dy = at.y - SDL_max(block->at.y - block->radius,
                                   SDL_min(block->at.y + block->radius, at.y));
        double length = sqrt(dx * dx + dy * dy);
        double ax, ay, tx, ty, into, toward;

        if (length < 1e-6)
        {
            continue;
        }

        // Away from the nearest point of its square.
        ax = dx / length;
        ay = dy / length;
        into = *vx * ax + *vy * ay;

        if (into < 0.0)
        {
            tx = -ay;
            ty = ax;
            toward = (target.x - at.x) * tx + (target.y - at.y) * ty;

            if (fabs(toward) < 1.0)
            {
                toward = (at.x - block->at.x) * tx + (at.y - block->at.y) * ty;
            }

            if (toward < 0.0)
            {
                tx = -tx;
                ty = -ty;
            }

            *vx -= into * (ax + tx);
            *vy -= into * (ay + ty);
        }

        if (SDL_max(fabs(dx), fabs(dy)) < AVOID_TOUCH)
        {
            *vx += ax * AVOID_PUSH;
            *vy += ay * AVOID_PUSH;
        }
    }
}

// Running a jump: straight along its line at full speed, eased back onto
// the line when off it. The run was checked clear, so nothing is steered
// round; in the air the command does nothing anyway.
static void SteerJump(const body_t *body, int target, k_command_t *command)
{
    k_vec2_t from = path.points[target - 1], to = path.points[target];
    double length = SDL_max(Distance(from, to), 1e-6);
    double ux = (to.x - from.x) / length, uy = (to.y - from.y) / length;
    double off = (body->y - from.y) * ux - (body->x - from.x) * uy;    // left of it
    double back = SDL_max(-JUMP_LINE_SPEED, SDL_min(off * JUMP_LINE_GAIN, JUMP_LINE_SPEED));

    Steer(body, ux * tuning.max_speed + uy * back, uy * tuning.max_speed - ux * back,
          NULL, true, command);
}

static void SteerAlongRoute(const body_t *body, k_command_t *command)
{
    int target = status.target;
    int last = path.num_points - 1;
    k_vec2_t at = { body->x, body->y };
    const double *face = aim;
    double distance, speed, vx, vy, jump_face;
    boolean steady;

    // The waypoint may be reached during the tics still waiting to run.
    if (target < TargetLimit()
     && Distance(at, path.points[target]) < AdvanceRadius(target))
    {
        ++target;
    }

    // Just landed from a jump: stop first.
    if (jump_phase == JUMP_SETTLE)
    {
        Steer(body, 0.0, 0.0, aim, false, command);
        return;
    }

    // Straight along the jump's line, facing it, never where combat aims:
    // full speed needs a facing within about 53 degrees of the way it runs
    // (strafing alone tops out at 13.3 units a tic).
    if (OnJump(target))
    {
        SteerJump(body, target, command);
        return;
    }

    if (target == dry_point)
    {
        SteerToStop(body, DrySpot(), aim, command);
        return;
    }

    distance = Distance(at, path.points[target]);
    speed = SpeedLimit(body, target);

    // Never past the goal within one tic.
    if (target == last)
    {
        speed = SDL_min(speed, distance);
    }

    // Past a careful drop that must be left slowly, still up on the ledge:
    // no faster than that.
    if (target > 0 && careful[target - 1] && body->onground
     && body->floor > CellFloor(path.cells[path.leg_cells[target - 1]]) + MAX_CLIMB
     && CarefulSpeed(target - 1) < CAREFUL_SPEED)
    {
        speed = SDL_min(speed, CarefulSpeed(target - 1));
    }

    // Into and out of a careful corner: off a ledge's turn, the momentum
    // of the leg before must not carry the player over the side while it
    // speeds up along the next (Doom II MAP24, 8-unit ledges at right
    // angles over a pit).
    steady = careful[target] || (target > 0 && careful[target - 1]);

    // Coming up to a jump's run-up while combat aims elsewhere: facing the
    // jump already, so the run starts at full speed (Doom II MAP02: turned
    // round at the start, the player left the edge at 13.2 units a tic and
    // fell short).
    if (aim != NULL && target < last && path.legs[target] == K_LEG_JUMP)
    {
        jump_face = atan2(path.points[target + 1].y - path.points[target].y,
                          path.points[target + 1].x - path.points[target].x);
        face = &jump_face;
    }

    if (distance > 1e-6)
    {
        vx = (path.points[target].x - at.x) / distance * speed;
        vy = (path.points[target].y - at.y) / distance * speed;
        AvoidWalls(body, &vx, &vy);
        AvoidBlocks(body, path.points[target], &vx, &vy);
        Steer(body, vx, vy, face, steady, command);
    }
    else
    {
        Steer(body, 0.0, 0.0, face, steady, command);
    }
}

// Whether use or an attack, pressed in the command about to be sent, would
// reach the action's line: the engine traces straight ahead from where the
// player is when the tic starts, after the tic's turn (64 units for use).
static boolean LineReaches(const body_t *body, const k_command_t *command, double reach)
{
    double angle = body->angle + command->turn / ANGLETURN_PER_RADIAN;
    double dx = cos(angle), dy = sin(angle);
    double ex = action.b.x - action.a.x, ey = action.b.y - action.a.y;
    double denominator = dx * ey - dy * ex;
    double t, u;

    if (fabs(denominator) < 1e-9)
    {
        return false;
    }

    // Ray: body + t * (dx, dy); portal: a + u * (b - a).
    t = ((action.a.x - body->x) * ey - (action.a.y - body->y) * ex) / denominator;
    u = ((action.a.x - body->x) * dy - (action.a.y - body->y) * dx) / denominator;

    return t > 0.0 && t <= reach && u > 0.02 && u < 0.98;
}

// Only a hitscan attack sets off a gun switch: P_LineAttack calls
// P_ShootSpecialLine for each line its trace crosses. That is the fist, the
// chainsaw and the bullet and shell weapons; the rockets, plasma and BFG
// balls are projectiles, which never do.
static boolean Hitscan(weapontype_t weapon)
{
    return weapon == wp_fist || weapon == wp_chainsaw || weapon == wp_pistol
        || weapon == wp_shotgun || weapon == wp_supershotgun || weapon == wp_chaingun;
}

static boolean CanFire(const player_t *player, weapontype_t weapon)
{
    ammotype_t ammo = weaponinfo[weapon].ammo;

    // The shotgun's slot brings up the super shotgun when the player has
    // it, which fires two shells.
    int need = weapon == wp_supershotgun
            || (weapon == wp_shotgun && player->weaponowned[wp_supershotgun]) ? 2 : 1;

    return player->weaponowned[weapon] && (ammo == am_noammo || player->ammo[ammo] >= need);
}

// The weapon to shoot a gun switch with: the one in hand if it will do,
// else a bullet or shell weapon with ammo, else the fist (its slot brings
// up the chainsaw when the player has it). Melee reaches 64 units, enough
// from the spot in front of the line. Each is picked by the slot of its
// own number.
static weapontype_t ShootingWeapon(const player_t *player)
{
    static const weapontype_t choices[] = { wp_pistol, wp_chaingun, wp_shotgun };
    int i;

    if (Hitscan(player->readyweapon) && CanFire(player, player->readyweapon))
    {
        return player->readyweapon;
    }

    for (i = 0; i < arrlen(choices); ++i)
    {
        if (CanFire(player, choices[i]))
        {
            return choices[i];
        }
    }

    return wp_fist;
}

// Up, and in its ready state, where pressing fire fires (A_WeaponReady).
static boolean WeaponReady(const player_t *player)
{
    return player->pendingweapon == wp_nochange
        && player->psprites[ps_weapon].state
           == &states[weaponinfo[player->readyweapon].readystate];
}

// Whether a sector a switch moves by its tag is held by a mover that has yet
// to let go of it (K_Graph_SectorBusy): pressing now would do nothing (Doom
// II MAP02: switch 102 pressed as door 30 closed by itself, the press lost
// and counted as one of the switch's uses). Not a manual door's own line,
// which moves the sector behind it, and reverses a closing door.
static boolean LineBusy(int line)
{
    int special = K_Specials_LineSpecial(line);
    int s;

    if (K_Specials_MoverTrigger(special) == K_TRIGGER_NONE || K_Specials_ManualDoor(special))
    {
        return false;
    }

    for (s = 0; s < numsectors; ++s)
    {
        if (sectors[s].tag == lines[line].tag && K_Graph_SectorBusy(s))
        {
            return true;
        }
    }

    return false;
}

// Pressing use on a door that is opening (or open) closes it, and on a
// busy lift does nothing; a switch does nothing while its sector is busy.
static boolean WorthPressing(void)
{
    k_mover_t mover;

    if (action.kind == K_ACTION_USE || action.kind == K_ACTION_SHOOT)
    {
        return !LineBusy(action.line);
    }

    mover = K_Graph_SectorMover(action.sector);

    if (action.kind == K_ACTION_DOOR)
    {
        return mover == K_MOVER_NONE || mover == K_MOVER_DOWN;
    }

    return mover == K_MOVER_NONE;
}

static void SteerAction(int tic, const body_t *body, k_command_t *command)
{
    k_vec2_t at = { body->x, body->y };
    double speed = sqrt(body->momx * body->momx + body->momy * body->momy);

    if (action.phase == PHASE_APPROACH)
    {
        // Face the line on the way, if it is close.
        SteerToStop(body, action.spot,
                    Distance(at, action.spot) < 96.0 ? &action.face : aim, command);
        return;
    }

    // Waiting for a door or lift, or riding one, the facing is free until
    // the next press; not on a ride that ends in a jump, nor at a gun switch
    // (fire is held facing it).
    SteerToHold(body, action.spot, aim != NULL && action.phase == PHASE_WAIT
                                   && !action.jump_ride
                                   && action.kind != K_ACTION_SHOOT ? aim : &action.face,
                command);

    // Fire is held long enough for any weapon to see it: the chainsaw reads
    // it only as each frame of its idle animation starts, every 4 tics
    // (Doom II MAP28: three 1-tic presses at a gun switch all fell between).
    if (action.kind == K_ACTION_SHOOT && action.phase == PHASE_WAIT
     && tic - action.press_tic < ATTACK_HOLD)
    {
        command->attack = 1;
        return;
    }

    // A hitscan weapon first, and fire only once it is up: never a rocket
    // fired while switching.
    if (action.phase == PHASE_AIM && action.kind == K_ACTION_SHOOT)
    {
        const player_t *player = &players[consoleplayer];
        weapontype_t weapon = ShootingWeapon(player);

        if (player->readyweapon != weapon)
        {
            command->change = 1;
            command->weapon = weapon;
            return;
        }

        if (tic - action.press_tic > 1 && speed < 4.0 && WeaponReady(player) && WorthPressing()
         && LineReaches(body, command, weapon == wp_fist || weapon == wp_chainsaw
                                       ? MELEE_REACH : SHOT_REACH))
        {
            command->attack = 1;
            action.phase = PHASE_WAIT;
            action.tics = 0;
            action.press_tic = tic;
            ++action.presses;
        }

        return;
    }

    // Use is released for a tic between presses, or the engine ignores it.
    if (action.phase == PHASE_AIM && action.kind != K_ACTION_RIDE
     && tic - action.press_tic > 1 && speed < 4.0
     && WorthPressing() && LineReaches(body, command, USE_REACH))
    {
        command->use = 1;
        action.phase = PHASE_WAIT;
        action.tics = 0;
        action.press_tic = tic;
        ++action.presses;
    }
}

int K_Nav_Predict(int tic, k_pose_t *pose)
{
    body_t body;

    if (!Predict(tic, &body))
    {
        return 0;
    }

    pose->x = body.x;
    pose->y = body.y;
    pose->angle = body.angle;
    pose->momx = body.momx;
    pose->momy = body.momy;
    pose->onground = body.onground;
    pose->frozen = body.frozen;
    pose->floor = body.floor;
    pose->cell = -1;
    return 1;
}

void K_Nav_BuildCommand(int tic, const double *aim_angle, k_command_t *command)
{
    body_t body;

    memset(command, 0, sizeof(*command));
    aim = aim_angle;

    if (!Predict(tic, &body))
    {
        return;
    }

    if (status.state == K_NAV_DRIVING)
    {
        if (action.kind != K_ACTION_NONE)
        {
            SteerAction(tic, &body, command);
        }
        else
        {
            SteerAlongRoute(&body, command);
        }
    }
    else if (aim != NULL)
    {
        // Standing still, it can still face where combat wants.
        Steer(&body, 0.0, 0.0, aim, false, command);
    }

    Record(tic, command, &body);
}

void K_Nav_BuildMoveCommand(int tic, double vx, double vy, const double *aim_angle,
                            k_command_t *command)
{
    body_t body;
    k_vec2_t ahead;

    memset(command, 0, sizeof(*command));
    aim = aim_angle;

    if (!Predict(tic, &body))
    {
        return;
    }

    ahead.x = body.x + vx * 8.0;
    ahead.y = body.y + vy * 8.0;
    AvoidWalls(&body, &vx, &vy);
    AvoidBlocks(&body, ahead, &vx, &vy);
    Steer(&body, vx, vy, aim, false, command);
    Record(tic, command, &body);
}

//
// Doors and lifts
//

static void Fail(const char *problem)
{
    status.state = K_NAV_FAILED;
    status.blocked = false;
    M_StringCopy(status.problem, problem, sizeof(status.problem));
}

static void ActionText(void)
{
    static const char *kinds[] = { "", "door", "lift", "lift ride", "switch", "gun switch" };

    if (action.line >= 0)
    {
        M_snprintf(status.action_text, sizeof(status.action_text), "%s at line %d: %s",
                   kinds[action.kind], action.line, phase_names[action.phase]);
    }
    else
    {
        M_snprintf(status.action_text, sizeof(status.action_text), "%s: %s",
                   kinds[action.kind], phase_names[action.phase]);
    }
}

static void SetPhase(phase_t phase)
{
    action.phase = phase;
    action.tics = 0;
    ActionText();
}

static double PlaneHeight(void)
{
    double floor, ceiling;

    K_Map_SectorHeights(action.sector, &floor, &ceiling);
    return action.kind == K_ACTION_DOOR ? ceiling : floor;
}

static void EndAction(void)
{
    action.kind = K_ACTION_NONE;
    status.action = K_ACTION_NONE;
    status.action_text[0] = '\0';
}

// A jump off a lift that is down (K_CROSS_RIDE): stand at the run-up's
// start, facing the way of the jump, and ride the lift until the jump
// works again.
static void StartJumpRide(int r)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &K_Graph_Get()->portals[path.portals[r]];
    const k_jump_t *jump = K_Jump_Get(portal->jump);

    memset(&action, 0, sizeof(action));
    action.kind = K_ACTION_RIDE;
    action.route_cell = r;
    action.line = -1;
    action.a = action.b = portal->a;
    action.spot = portal->a;
    action.face = atan2(jump->dir.y, jump->dir.x);
    action.jump_ride = true;
    action.start_tics = status.tics;
    action.press_tic = -2;
    action.sector = map->subsectors[map->cells[path.cells[r]].subsector].sector;
    action.height = PlaneHeight();
    action.mover = K_Graph_SectorMover(action.sector);

    status.action = action.kind;
    status.action_line = -1;
    SetPhase(PHASE_APPROACH);
    K_Log(K_LOG_NAV, K_LOG_INFO, "riding the lift (sector %d) back up for the jump",
          action.sector);

    // The run-up may have begun as the lift went down (stepping on it set it
    // going): back to its start; the lift sinking under the player is no
    // takeoff.
    status.target = SDL_min(status.target, ExitPoint(r));
    jump_phase = JUMP_NONE;
}

// The crossing out of route cell r needs a door or lift.
static void StartAction(k_crossing_t crossing, int r)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &K_Graph_Get()->portals[path.portals[r]];
    int from = path.cells[r], to = path.cells[r + 1];

    if (portal->jump >= 0)
    {
        StartJumpRide(r);
        return;
    }
    k_vec2_t middle = { (portal->a.x + portal->b.x) * 0.5,
                        (portal->a.y + portal->b.y) * 0.5 };
    k_vec2_t inside = map->cells[from].centroid;
    double length = Distance(portal->a, portal->b);
    double nx = -(portal->b.y - portal->a.y) / length;
    double ny = (portal->b.x - portal->a.x) / length;

    // The normal that points back into the cell the player is in.
    if ((inside.x - middle.x) * nx + (inside.y - middle.y) * ny < 0.0)
    {
        nx = -nx;
        ny = -ny;
    }

    memset(&action, 0, sizeof(action));
    action.kind = crossing == K_CROSS_DOOR ? K_ACTION_DOOR
                : crossing == K_CROSS_LIFT ? K_ACTION_LIFT : K_ACTION_RIDE;
    action.route_cell = r;
    action.line = portal->line;
    action.a = portal->a;
    action.b = portal->b;

    // The whole line, not just the portal: a doorway can be split into
    // several portals, and standing in front of one at its edge can put
    // the spot closer to the side wall than the player's radius. Doors and
    // lifts sit centred, so in front of the line's middle is clear.
    if (portal->line >= 0)
    {
        const k_line_t *line = &map->lines[portal->line];
        k_vec2_t centre = { (line->v1.x + line->v2.x) * 0.5,
                            (line->v1.y + line->v2.y) * 0.5 };

        if (K_Map_CellAt(centre.x + nx * ACTION_DISTANCE,
                         centre.y + ny * ACTION_DISTANCE) >= 0)
        {
            action.a = line->v1;
            action.b = line->v2;
            middle = centre;
        }
    }

    action.spot.x = middle.x + nx * ACTION_DISTANCE;
    action.spot.y = middle.y + ny * ACTION_DISTANCE;
    action.face = atan2(-ny, -nx);
    action.start_tics = status.tics;
    action.press_tic = -2;

    // A ride watches the lift the player is on; the others what is ahead.
    action.sector = map->subsectors[map->cells[action.kind == K_ACTION_RIDE
                                               ? from : to].subsector].sector;
    action.height = PlaneHeight();
    action.mover = K_Graph_SectorMover(action.sector);

    status.action = action.kind;
    status.action_line = action.line;
    SetPhase(PHASE_APPROACH);

    switch (action.kind)
    {
        case K_ACTION_DOOR:
            K_Log(K_LOG_NAV, K_LOG_INFO, "opening the door at line %d (%.0f,%.0f)",
                  action.line, middle.x, middle.y);
            break;

        case K_ACTION_LIFT:
            if (K_Graph_SectorMover(action.sector) != K_MOVER_NONE)
            {
                K_Log(K_LOG_NAV, K_LOG_INFO, "waiting for the lift (sector %d) to come "
                      "down at line %d", action.sector, action.line);
            }
            else
            {
                K_Log(K_LOG_NAV, K_LOG_INFO, "calling the lift at line %d (%.0f,%.0f)",
                      action.line, middle.x, middle.y);
            }
            break;

        default:
            K_Log(K_LOG_NAV, K_LOG_INFO, "riding the lift (sector %d) to the way off",
                  action.sector);
            break;
    }
}

static void FinishAction(void)
{
    double seconds = (double) (status.tics - action.start_tics) / TICRATE;

    switch (action.kind)
    {
        case K_ACTION_DOOR:
            K_Log(K_LOG_NAV, K_LOG_INFO, "door at line %d open after %.1f s",
                  action.line, seconds);
            break;

        case K_ACTION_LIFT:
            K_Log(K_LOG_NAV, K_LOG_INFO, "lift at line %d down after %.1f s",
                  action.line, seconds);
            break;

        default:
            K_Log(K_LOG_NAV, K_LOG_INFO, "lift ride over after %.1f s", seconds);
            K_Graph_LiftRidden(action.sector);
            break;
    }

    EndAction();
}

static void FailAction(const char *what)
{
    char problem[sizeof(status.problem)];

    if (action.line >= 0)
    {
        M_snprintf(problem, sizeof(problem), "%s at line %d", what, action.line);
    }
    else
    {
        M_snprintf(problem, sizeof(problem), "%s (sector %d)", what, action.sector);
    }

    K_Log(K_LOG_NAV, K_LOG_WARN, "%s", problem);
    EndAction();
    Fail(problem);
}

// At the goal: press, or shoot, the switch the player was sent to.
static void StartUseAction(void)
{
    const k_line_t *line = &K_Map_Get()->lines[status.goal_line];
    k_vec2_t middle = { (line->v1.x + line->v2.x) * 0.5, (line->v1.y + line->v2.y) * 0.5 };

    memset(&action, 0, sizeof(action));
    action.kind = status.goal_kind == K_GOAL_SHOOT_LINE ? K_ACTION_SHOOT : K_ACTION_USE;
    action.route_cell = -1;
    action.sector = -1;
    action.line = status.goal_line;
    action.a = line->v1;
    action.b = line->v2;
    action.spot = goal;
    action.face = atan2(middle.y - goal.y, middle.x - goal.x);
    action.start_tics = status.tics;
    action.press_tic = -2;

    status.action = action.kind;
    status.action_line = action.line;
    SetPhase(PHASE_AIM);

    K_Log(K_LOG_NAV, K_LOG_INFO, "%s the switch at line %d (%.0f,%.0f)",
          action.kind == K_ACTION_SHOOT ? "shooting" : "pressing", action.line,
          middle.x, middle.y);
}

// What a switch does is up to whoever sent the player (an exit ends the
// level); here, only a press that changed nothing is repeated.
static boolean UpdateUseAction(void)
{
    ++action.tics;

    // Its sector still on its way: wait at the switch, then press.
    if (action.phase == PHASE_AIM && LineBusy(action.line))
    {
        if (status.tics - action.start_tics > BUSY_TICS)
        {
            FailAction("what the switch moves stays busy");
            return false;
        }

        action.tics = 0;
        return true;
    }

    if (action.phase == PHASE_AIM && action.tics > AIM_TICS)
    {
        FailAction(action.kind == K_ACTION_SHOOT ? "could not shoot the line"
                                                 : "could not use the line");
        return false;
    }

    if (action.phase == PHASE_WAIT && gametic - action.press_tic > PRESS_TICS)
    {
        if (action.presses >= MAX_PRESSES)
        {
            FailAction(action.kind == K_ACTION_SHOOT ? "shooting the line does nothing"
                                                     : "using the line does nothing");
            return false;
        }

        SetPhase(PHASE_AIM);
    }

    return true;
}

static boolean Replan(k_vec2_t at, const char *why);
static boolean ShortOfLine(k_vec2_t at);

// Works the door or lift at the next crossing that is not open, if the
// player has got to it. Returns false if the route was replaced or failed.
static boolean UpdateAction(k_vec2_t at, double speed)
{
    int r;
    k_crossing_t crossing;
    k_mover_t mover;
    double height;
    char why[96];

    if (action.kind == K_ACTION_USE || action.kind == K_ACTION_SHOOT)
    {
        return UpdateUseAction();
    }

    r = NextClosedCrossing();

    if (action.kind != K_ACTION_NONE && action.route_cell != r)
    {
        // The crossing it worked on is open now: done. (Or the player was
        // pushed back to an earlier one: start over there.)
        if (r < 0 || r > action.route_cell)
        {
            FinishAction();
        }
        else
        {
            EndAction();
        }
    }

    if (r < 0)
    {
        return true;
    }

    crossing = CrossingOut(r);

    // A teleporter needs nothing but running over its line, once anything
    // it waits for is over (a ride up to it, Doom II MAP03), as long as it
    // still works: a one-shot one is used up once crossed.
    if (TeleportOut(r) && crossing == K_CROSS_TELEPORT)
    {
        if (action.kind != K_ACTION_NONE)
        {
            FinishAction();
        }

        return true;
    }

    if (TeleportOut(r) && (crossing == K_CROSS_OPEN || !K_Graph_Passable(crossing)))
    {
        EndAction();
        M_snprintf(why, sizeof(why), "the teleporter at line %d is %s%s",
                   K_Graph_Get()->portals[path.portals[r]].line,
                   crossing == K_CROSS_OPEN ? "used up" : "blocked: ",
                   crossing == K_CROSS_OPEN ? "" : K_Graph_CrossingName(crossing));
        return Replan(at, why);
    }

    if (status.target < ExitPoint(r))
    {
        return true;
    }

    // The way ahead closed since the route was planned, e.g. a door that
    // no longer opens high enough.
    if (!K_Graph_Passable(crossing))
    {
        EndAction();
        M_snprintf(why, sizeof(why), "%s ahead at (%.0f,%.0f)",
                   K_Graph_CrossingName(crossing), path.points[status.target].x,
                   path.points[status.target].y);
        return Replan(at, why);
    }

    if (action.kind == K_ACTION_NONE)
    {
        StartAction(crossing, r);
    }

    ++action.tics;
    height = PlaneHeight();
    action.still = height != action.height ? 0 : action.still + 1;
    action.height = height;
    mover = K_Graph_SectorMover(action.sector);

    // A lift that turns straight back down while the player rides it,
    // before it can get off (at the top, a lift stops or waits first):
    // something the map does not show is in its way, a monster at its edge
    // or a barrel. Never given up on: the route is planned again with that
    // lift dearer, so it goes another way if one is worth it, and else
    // rides again, however often the lift turns.
    if (action.kind == K_ACTION_RIDE && action.mover == K_MOVER_UP
     && mover == K_MOVER_DOWN)
    {
        M_snprintf(why, sizeof(why), "the lift (sector %d) turned back at %.0f, short of "
                   "the way off (%d so far)", action.sector, height,
                   K_Graph_LiftTurnedBack(action.sector));
        EndAction();
        return Replan(at, why);
    }

    action.mover = mover;

    switch (action.phase)
    {
        case PHASE_APPROACH:
            if (Distance(at, action.spot) < SPOT_RADIUS && speed < 1.5)
            {
                SetPhase(action.kind == K_ACTION_RIDE ? PHASE_WAIT : PHASE_AIM);
            }
            else if (action.tics > APPROACH_TICS)
            {
                // Somewhere near is good enough: the spot may be in a wall.
                SetPhase(action.kind == K_ACTION_RIDE ? PHASE_WAIT : PHASE_AIM);
            }
            break;

        case PHASE_AIM:
            // Already moving the right way: no press needed (Act presses
            // only when it is worth it).
            if ((action.kind == K_ACTION_DOOR
              && (mover == K_MOVER_UP || mover == K_MOVER_WAITING))
             || (action.kind == K_ACTION_LIFT && mover != K_MOVER_NONE))
            {
                SetPhase(PHASE_WAIT);
            }
            else if (action.tics > AIM_TICS)
            {
                FailAction("could not use the line");
                return false;
            }
            break;

        case PHASE_WAIT:
            if (action.kind == K_ACTION_DOOR && mover == K_MOVER_DOWN)
            {
                // Closing before the player got through: pressing again
                // reopens it.
                SetPhase(PHASE_AIM);
            }
            else if (action.kind != K_ACTION_RIDE && mover == K_MOVER_NONE
                  && gametic - action.press_tic > PRESS_TICS)
            {
                // Nothing moves: the press missed, or a lift went back up
                // before the player stepped on.
                if (action.presses >= MAX_PRESSES)
                {
                    FailAction("using the line does nothing");
                    return false;
                }

                SetPhase(PHASE_AIM);
            }
            else if (action.still > STALL_TICS)
            {
                FailAction(action.kind == K_ACTION_DOOR ? "the door stopped short"
                         : action.kind == K_ACTION_LIFT ? "the lift does not come down"
                         : "the lift does not rise");
                return false;
            }
            break;
    }

    return true;
}

//
// Following the route
//

// For a cell off the route but one open step from it (e.g. a corner cut
// through a neighbouring cell): the position of that route cell, or -1.
static int RouteNeighbor(int cell)
{
    const k_graph_t *graph = K_Graph_Get();
    int i, end = graph->first_link[cell] + graph->link_count[cell];

    for (i = graph->first_link[cell]; i < end; ++i)
    {
        int r = RouteIndex(graph->links[i].neighbor);

        if (r >= 0 && K_Graph_CanCross(graph->links[i].portal, cell) == K_CROSS_OPEN)
        {
            return r;
        }
    }

    return -1;
}

// Whether a crossing from one cell into a neighbouring one is open.
static boolean OpenTo(int cell, int to)
{
    const k_graph_t *graph = K_Graph_Get();
    int i, end = graph->first_link[cell] + graph->link_count[cell];

    for (i = graph->first_link[cell]; i < end; ++i)
    {
        if (graph->links[i].neighbor == to
         && K_Graph_CanCross(graph->links[i].portal, cell) == K_CROSS_OPEN)
        {
            return true;
        }
    }

    return false;
}

static double CellFloor(int cell)
{
    return K_Map_CellFloor(cell);
}

// Which route points to take carefully: near an edge, where the route
// turns or steps down. The edge is judged from the floor before the point.
static void MarkCareful(void)
{
    double turn_cos = cos(CAREFUL_TURN * K_PI / 180.0);
    int k;

    if (path.num_points > careful_capacity)
    {
        careful_capacity = path.num_points * 2;
        careful = realloc(careful, careful_capacity);
    }

    memset(careful, 0, path.num_points);

    for (k = 1; k < path.num_points - 1; ++k)
    {
        k_vec2_t a = path.points[k - 1], b = path.points[k], c = path.points[k + 1];
        double in = Distance(a, b), out = Distance(b, c);
        double before = CellFloor(path.cells[path.leg_cells[k - 1]]);
        double after = CellFloor(path.cells[path.leg_cells[k]]);
        boolean turns = in > 1e-6 && out > 1e-6
                     && ((b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y))
                        / (in * out) < turn_cos;

        if (path.legs[k] != K_LEG_WALK || path.legs[k - 1] != K_LEG_WALK
         || (!turns && after >= before))
        {
            continue;
        }

        careful[k] = K_Graph_DropDistance(b, before, CAREFUL_REACH) < CAREFUL_REACH;
    }
}

static void StartRoute(void)
{
    MarkCareful();
    dry_point = -1;
    status.target = 1;
    status.route_cell = 0;
    status.remaining = path.length;
    history_count = 0;
    history_next = 0;
    jump_phase = JUMP_NONE;
    EndAction();
}

// A new route from where the player is. Returns false (and fails) if
// there is none.
static boolean Replan(k_vec2_t at, const char *why)
{
    char text[K_LOG_LINE_LEN];
    char problem[sizeof(status.problem)];
    k_pose_t pose;

    ++status.replans;
    K_Path_Find(&path, at, K_Map_GetPlayer(&pose) ? pose.cell : -1, goal);
    K_Path_Describe(&path, text, sizeof(text));

    if (path.result != K_PATH_FOUND)
    {
        K_Log(K_LOG_NAV, K_LOG_WARN, "%s - no path: %s", why, text);
        M_snprintf(problem, sizeof(problem), "no path: %s", text);
        Fail(problem);
        status.blocked = path.result == K_PATH_BLOCKED;
        return false;
    }

    K_Log(K_LOG_NAV, K_LOG_INFO, "%s - replanned: %s", why, text);
    StartRoute();
    return true;
}

// Moves the target along as the player goes, by the cell it stands on.
// Returns false if the route was replaced or failed. Off the route but
// next to it, the player carries on; teleported, it goes on from that
// route cell (the route cell it was in is far away), else it replans.
// Running a jump, it crosses cells off the route, or earlier on it (the
// rest of the run-up's floor, the gap), until it lands: only getting on,
// on the ground, counts (Doom II MAP24: a run-up across an earlier route
// cell turned the player back from the edge; flying over a later one, the
// corner after the landing was cut, straight over the nukage). On the
// ground more than a step below the floor it lands on, it is down in the
// gap: it missed.
static boolean Progress(k_vec2_t at, const k_pose_t *pose, boolean teleported)
{
    int cell = pose->cell;
    int r = RouteIndex(cell);
    int limit;
    char why[96];

    if ((r <= status.route_cell || !pose->onground) && OnJump(status.target))
    {
        double landing = CellFloor(path.cells[path.leg_cells[status.target - 1]]);

        if (pose->onground && pose->floor < landing - MAX_CLIMB)
        {
            M_snprintf(why, sizeof(why), "missed the jump: down at (%.0f,%.0f), %.0f below",
                       at.x, at.y, landing - pose->floor);
            return Replan(at, why);
        }

        return true;
    }

    // The move that crosses a teleport line goes on from the destination
    // (P_XYMovement moves in steps), so at speed the player lands a few
    // units past it, possibly in the next cell.
    if (r < 0 && teleported && cell >= 0)
    {
        r = RouteNeighbor(cell);
    }

    if (r > status.route_cell)
    {
        // Further along (perhaps having cut a corner): never back.
        status.target = SDL_max(status.target, FirstPointIn(r));
        status.route_cell = r;
    }
    else if (r >= 0 && r < status.route_cell
          && !OpenTo(cell, path.cells[status.route_cell]))
    {
        // Pushed back: drive out of this cell again. Not if it only slipped
        // into an earlier route cell lying alongside, and can walk straight
        // on into the route cell it was in (Doom II MAP26: a sliver along a
        // ledge's edge beside two later route cells, where the bot turned
        // back 150 units for a drift of a unit or two).
        K_Log(K_LOG_NAV, K_LOG_INFO, "back in route cell %d of %d at (%.0f,%.0f) - "
              "going on from there", r, path.num_cells, at.x, at.y);
        status.target = FirstPointIn(r);
        status.route_cell = r;
    }
    else if (r < 0 && cell >= 0 && (teleported || RouteNeighbor(cell) < 0))
    {
        M_snprintf(why, sizeof(why), "off the route in cell %d at (%.0f,%.0f)",
                   cell, at.x, at.y);
        return Replan(at, why);
    }

    limit = TargetLimit();

    while (status.target < limit
        && Distance(at, path.points[status.target]) < AdvanceRadius(status.target))
    {
        ++status.target;
    }

    return true;
}

// Whether the floor the player stands on is sinking (Doom II MAP08's
// pillars).
static boolean Sinking(const k_pose_t *pose)
{
    const k_map_t *map = K_Map_Get();

    return pose->cell >= 0
        && K_Graph_FloorSinking(map->subsectors[map->cells[pose->cell].subsector].sector);
}

// Landed: stop first, unless the floor is sinking, when stopping would
// leave the next jump too high to make.
static void Settle(const k_pose_t *pose)
{
    jump_phase = Sinking(pose) ? JUMP_NONE : JUMP_SETTLE;
    settle_tics = 0;
}

// A jump's run-up, takeoff, landing and stop, in the log. Off the ground
// over a floor more than a step below the run-up's is the takeoff; a step
// down on the way is still the run-up (Doom II MAP20: the walkway down to
// the edge). Touching down ends the flight: the target moves on, and the
// player stops first. A step across a gap narrower than the player never
// leaves the ground: it ends when the target moves past the touchdown.
static void TrackJump(const k_pose_t *pose, k_vec2_t at)
{
    // Waiting for a lift to bring the jump back is no part of it.
    if (action.kind != K_ACTION_NONE)
    {
        return;
    }

    switch (jump_phase)
    {
        case JUMP_NONE:
            if (OnJump(status.target))
            {
                K_Log(K_LOG_NAV, K_LOG_INFO, "running up for the jump to (%.0f,%.0f)",
                      path.points[status.target].x, path.points[status.target].y);
                jump_phase = JUMP_RUNUP;
                runup_floor = pose->floor;
            }
            break;

        case JUMP_RUNUP:
            if (!OnJump(status.target))
            {
                Settle(pose);
            }
            else if (pose->onground)
            {
                runup_floor = pose->floor;
            }
            else if (pose->floor < runup_floor - MAX_CLIMB)
            {
                K_Log(K_LOG_NAV, K_LOG_INFO, "off the edge at (%.0f,%.0f) at %.1f units a tic",
                      at.x, at.y, status.speed);
                jump_phase = JUMP_AIR;
            }
            break;

        case JUMP_AIR:
            if (pose->onground && OnJump(status.target)
             && (fabs(pose->floor - CellFloor(path.cells[path.leg_cells[status.target - 1]])) > 1.0
              || Distance(at, path.points[status.target]) > LANDING_REACH))
            {
                break;
            }

            if (pose->onground)
            {
                K_Log(K_LOG_NAV, K_LOG_INFO, "landed at (%.0f,%.0f), floor %.0f, at %.1f "
                      "units a tic%s", at.x, at.y, pose->floor, status.speed,
                      Sinking(pose) ? " - the floor is sinking, running on" : "");

                if (OnJump(status.target))
                {
                    ++status.target;
                }

                Settle(pose);
            }
            break;

        case JUMP_SETTLE:
            if (status.speed < SETTLE_SPEED || ++settle_tics > SETTLE_TICS)
            {
                jump_phase = JUMP_NONE;
            }
            break;
    }
}

// Whether the player has hardly moved over the last STUCK_TICS tics.
static boolean Stuck(k_vec2_t at)
{
    k_vec2_t then = history[history_next];

    history[history_next] = at;
    history_next = (history_next + 1) % STUCK_TICS;

    if (history_count < STUCK_TICS)
    {
        ++history_count;
        return false;
    }

    return Distance(at, then) < STUCK_DISTANCE;
}

static double Remaining(k_vec2_t at)
{
    double remaining = Distance(at, path.points[status.target]);
    int k;

    for (k = status.target; k < path.num_points - 1; ++k)
    {
        if (path.legs[k] != K_LEG_TELEPORT)
        {
            remaining += Distance(path.points[k], path.points[k + 1]);
        }
    }

    return remaining;
}

static boolean CellHurts(int cell)
{
    const k_map_t *map = K_Map_Get();

    return K_Map_FloorDamage(map->subsectors[map->cells[cell].subsector].sector) > 0.0;
}

// How long the way from route point `from` to `to` takes at the most, as
// waiting dry times it (DRY_SPEED).
static double TravelTics(int from, int to)
{
    double tics = 0.0;
    int k;

    for (k = from; k < to; ++k)
    {
        double fall = CellFloor(path.cells[path.leg_cells[k - 1]])
                    - CellFloor(path.cells[path.leg_cells[k]]);

        tics += Distance(path.points[k], path.points[k + 1]) / DRY_SPEED;

        if (fall > MAX_CLIMB)
        {
            tics += sqrt(2.0 * fall) + 1.0;
        }
    }

    return tics;
}

// Whether the player waits dry, and where (dry_point): the first crossing
// ahead that is not open opens by itself (K_Graph_OpensByItself), the cell
// in front of it hurts, and so do the route cells back to the last dry one,
// the player's or one ahead, walked from there; and the wait outlasts the
// way there by DRY_MARGIN (to start, by DRY_SAVING more). Doom II MAP20:
// after line 557 the bot dropped off the walkway into the blood in front of
// floor 26, and stood there 10 s while it came down. Judged again every
// tic: where the player has to run for it at once, through the floor that
// hurts, to get there in time, it never waits.
static void UpdateDry(void)
{
    const k_graph_t *graph = K_Graph_Get();
    int was = dry_point;
    int r = NextClosedCrossing();
    int start = SDL_max(status.route_cell, 0);
    double wait = 0.0, travel = 0.0;
    int d, k, point, exit_point;

    dry_point = -1;

    if (r >= 0 && action.kind == K_ACTION_NONE && jump_phase == JUMP_NONE
     && !TeleportOut(r) && CellHurts(path.cells[r])
     && K_Graph_OpensByItself(path.portals[r], path.cells[r]))
    {
        for (d = r - 1; d >= start && CellHurts(path.cells[d]); --d)
        {
        }

        point = d >= start ? ExitPoint(d) : 0;
        exit_point = ExitPoint(r);

        for (k = point - 1; k >= 0 && k < exit_point && path.legs[k] == K_LEG_WALK; ++k)
        {
        }

        if (point >= 1 && k == exit_point)
        {
            dry_back = SDL_min(DRY_BACK,
                               0.5 * Distance(path.points[point - 1], path.points[point]));
            wait = K_Graph_CrossingWait(path.portals[r], path.cells[r], NULL);
            travel = TravelTics(point, exit_point) + dry_back / DRY_SPEED;

            if (wait > travel + DRY_MARGIN + (was == point ? 0.0 : DRY_SAVING))
            {
                dry_point = point;
            }
        }
    }

    if (dry_point >= 0 && was != dry_point)
    {
        k_vec2_t spot = DrySpot();

        dry_since = status.tics;
        K_Log(K_LOG_NAV, K_LOG_INFO, "waiting at (%.0f,%.0f), off the floor that hurts "
              "ahead, for the %s at line %d: open in %.1f s, %.1f s away", spot.x, spot.y,
              CrossingOut(r) == K_CROSS_DOOR ? "door" : "lift",
              graph->portals[path.portals[r]].line, wait / TICRATE, travel / TICRATE);
    }
    else if (dry_point < 0 && was >= 0)
    {
        K_Log(K_LOG_NAV, K_LOG_INFO, "going on after waiting dry %.1f s",
              (double) (status.tics - dry_since) / TICRATE);
    }
}

k_navstate_t K_Nav_Tick(void)
{
    k_pose_t pose;
    k_vec2_t at;
    boolean teleported;
    char text[96];

    if (status.state != K_NAV_DRIVING || !K_Map_GetPlayer(&pose))
    {
        return status.state;
    }

    // Fighting: the route waits, and the time spent is no sign of being
    // stuck.
    if (paused)
    {
        history_count = 0;
        history_next = 0;
        return status.state;
    }

    at.x = pose.x;
    at.y = pose.y;
    ++status.tics;
    status.speed = sqrt(pose.momx * pose.momx + pose.momy * pose.momy);
    teleported = Distance(at, last_at) > TELEPORT_JUMP;

    if (teleported)
    {
        // No misprediction, and no progress to measure from before.
        K_Log(K_LOG_NAV, K_LOG_INFO, "teleported from (%.0f,%.0f) to (%.0f,%.0f)",
              last_at.x, last_at.y, at.x, at.y);
        history_count = 0;
        history_next = 0;
    }
    else
    {
        CheckPrediction(at);

        // Off a ledge, on purpose or not: no way back up the same way. (Not
        // over a jump's gap, which it flies across.)
        if (last_floor - pose.floor > MAX_CLIMB && jump_phase == JUMP_NONE)
        {
            K_Log(K_LOG_NAV, K_LOG_INFO, "dropped %.0f units at (%.0f,%.0f), into cell %d",
                  last_floor - pose.floor, at.x, at.y, pose.cell);
        }
    }

    last_at = at;
    last_floor = pose.floor;
    UpdateDry();

    if (!Progress(at, &pose, teleported) || !UpdateAction(at, status.speed))
    {
        return status.state;
    }

    TrackJump(&pose, at);

    if (status.target == path.num_points - 1 && Distance(at, goal) < ARRIVE_RADIUS
     && status.speed < ARRIVE_SPEED && action.kind == K_ACTION_NONE && !ShortOfLine(at))
    {
        if (status.goal_kind == K_GOAL_USE_LINE || status.goal_kind == K_GOAL_SHOOT_LINE)
        {
            StartUseAction();
        }
        else if (cross_first)
        {
            cross_first = false;
            goal = cross_to;
            M_snprintf(text, sizeof(text), "in front of line %d, walking across",
                       status.goal_line);

            if (!Replan(at, text))
            {
                return status.state;
            }
        }
        else
        {
            have_goal = false;
            status.state = K_NAV_IDLE;
            status.remaining = 0.0;
            return K_NAV_ARRIVED;
        }
    }

    if (status.stuck > 0 && Distance(at, stuck_at) > UNSTUCK_DISTANCE)
    {
        status.stuck = 0;
    }

    // Standing still is the point while working a door or lift, or waiting
    // dry; the action has its own time limits, and the wait its end.
    if ((action.kind != K_ACTION_NONE && action.phase != PHASE_APPROACH) || dry_point >= 0)
    {
        history_count = 0;
        history_next = 0;
    }
    else if (Stuck(at))
    {
        stuck_at = at;

        if (++status.stuck > MAX_STUCK)
        {
            M_snprintf(text, sizeof(text), "stuck at (%.0f,%.0f)", at.x, at.y);
            EndAction();
            Fail(text);
            return status.state;
        }

        M_snprintf(text, sizeof(text), "stuck at (%.0f,%.0f) for %d tics (%d/%d)",
                   at.x, at.y, STUCK_TICS, status.stuck, MAX_STUCK);

        if (!Replan(at, text))
        {
            return status.state;
        }
    }

    status.remaining = Remaining(at);
    return status.state;
}

//
// Goals
//

// Whether the player, at a point, has yet to cross the walk-over line it is
// walking across: its goal can be nearer the line than the player stops from
// it (K_Nav_LineGoalPoint).
static boolean ShortOfLine(k_vec2_t at)
{
    const k_line_t *line;
    double dx, dy;

    if (status.goal_kind != K_GOAL_CROSS_LINE || cross_first || status.goal_line < 0)
    {
        return false;
    }

    line = &K_Map_Get()->lines[status.goal_line];
    dx = line->v2.x - line->v1.x;
    dy = line->v2.y - line->v1.y;

    return (dx * (at.y - line->v1.y) - dy * (at.x - line->v1.x))
         * (dx * (goal.y - line->v1.y) - dy * (goal.x - line->v1.x)) <= 0.0;
}

void K_Nav_Reset(void)
{
    int i;

    have_goal = false;
    cross_first = false;
    dry_point = -1;
    paused = false;
    memset(&status, 0, sizeof(status));
    status.state = K_NAV_IDLE;
    jump_phase = JUMP_NONE;
    EndAction();

    for (i = 0; i < SENT_SIZE; ++i)
    {
        sent[i].tic = -1;
    }
}

// Whether the route crosses the line.
static boolean RouteCrosses(int line)
{
    const k_graph_t *graph = K_Graph_Get();
    int i;

    for (i = 0; i < path.num_cells - 1; ++i)
    {
        if (graph->portals[path.portals[i]].line == line)
        {
            return true;
        }
    }

    return false;
}

// The point as far on the other side of the line.
static k_vec2_t Mirror(int line_index, k_vec2_t point)
{
    const k_line_t *line = &K_Map_Get()->lines[line_index];
    double dx = line->v2.x - line->v1.x, dy = line->v2.y - line->v1.y;
    double t = ((point.x - line->v1.x) * dx + (point.y - line->v1.y) * dy)
             / SDL_max(dx * dx + dy * dy, 1e-12);
    k_vec2_t result;

    result.x = 2.0 * (line->v1.x + dx * t) - point.x;
    result.y = 2.0 * (line->v1.y + dy * t) - point.y;
    return result;
}

static int StartGoal(double x, double y, k_goalkind_t kind, int line)
{
    char text[K_LOG_LINE_LEN];
    k_vec2_t start;
    k_pose_t player;

    if (!K_Map_GetPlayer(&player))
    {
        have_goal = false;
        status.state = K_NAV_IDLE;
        return 0;
    }

    start.x = player.x;
    start.y = player.y;
    goal.x = x;
    goal.y = y;
    last_at = start;
    last_floor = player.floor;
    cross_first = false;

    K_Path_Find(&path, start, player.cell, goal);

    // A walk-over line works only if the player crosses it, but the route
    // to the point past it can go round its end (Doom II MAP05). Then walk
    // up to the point before it first.
    if (kind == K_GOAL_CROSS_LINE && path.result == K_PATH_FOUND && !RouteCrosses(line))
    {
        k_vec2_t before = Mirror(line, goal);

        if (K_Map_CellAt(before.x, before.y) >= 0)
        {
            K_Log(K_LOG_NAV, K_LOG_INFO, "the way past line %d goes round it - "
                  "walking up to it first", line);
            cross_first = true;
            cross_to = goal;
            goal = before;
            K_Path_Find(&path, start, player.cell, goal);
        }
    }

    have_goal = true;

    memset(&status, 0, sizeof(status));
    status.state = K_NAV_DRIVING;
    status.goal_kind = kind;
    status.goal_line = line;
    StartRoute();

    K_Path_Describe(&path, text, sizeof(text));

    if (path.result == K_PATH_FOUND)
    {
        K_Log(K_LOG_NAV, K_LOG_INFO, "path found: %s, %d waypoints - %.2f ms, "
              "%d nodes searched", text, path.num_points - 1, path.search_ms,
              path.searched);
        return 1;
    }

    K_Log(K_LOG_NAV, K_LOG_WARN, "no path from (%.0f,%.0f): %s - %.2f ms, "
          "%d nodes searched", start.x, start.y, text, path.search_ms,
          path.searched);
    M_snprintf(status.problem, sizeof(status.problem), "no path: %s", text);
    status.state = K_NAV_FAILED;
    return 0;
}

int K_Nav_SetGoal(double x, double y)
{
    return StartGoal(x, y, K_GOAL_POINT, -1);
}

// Whether the player's box at a point, with a unit to spare, overlaps no
// solid thing.
static boolean ClearOfBlocks(double x, double y)
{
    k_vec2_t min = { x - STAND_CLEARANCE, y - STAND_CLEARANCE };
    k_vec2_t max = { x + STAND_CLEARANCE, y + STAND_CLEARANCE };
    int found;

    return K_Map_BlocksNear(min, max, &found, 1) == 0;
}

int K_Nav_LineGoalPoint(int line_index, k_goalkind_t kind, double *x, double *y)
{
    // Tried in turn until one is on floor, clear of solid things: a switch
    // may sit in a shallow alcove, a walk-over line just in front of a
    // wall. A walk-over line can also be crossed off its middle, so the
    // places along it are tried too, from the middle out (Doom II MAP27: a
    // skull rock inside a diamond of walk-over lines, 11 units from each).
    // If nothing is clear of solid things, the first spot on floor.
    // A walk-over line is crossed once the player's centre is past it, so
    // first a spot the player can stand on is looked for, nearer the line
    // too (Doom II MAP04: a box in a corner, 8 units from both walls, whose
    // sides are crossed only in a square 8 across on top of it; 24 past them
    // is a wall).
    static const double distances[] = { 24.0, 32.0, 16.0, 40.0 };
    static const double cross_distances[] = { 24.0, 32.0, 16.0, 40.0, 8.0, 4.0 };
    static const double alongs[] = { 0.5, 0.3, 0.7, 0.15, 0.85, 0.05, 0.95 };
    const k_line_t *line = &K_Map_Get()->lines[line_index];
    k_vec2_t middle = { (line->v1.x + line->v2.x) * 0.5, (line->v1.y + line->v2.y) * 0.5 };
    double length = Distance(line->v1, line->v2);
    int num_alongs = kind == K_GOAL_CROSS_LINE ? arrlen(alongs) : 1;
    boolean on_floor = false;
    double floor_x = 0.0, floor_y = 0.0;
    double nx, ny;
    k_pose_t player;
    int i, a;

    if (length < 1e-6)
    {
        return 0;
    }

    // The front side is on the right of v1 -> v2.
    nx = (line->v2.y - line->v1.y) / length;
    ny = -(line->v2.x - line->v1.x) / length;

    // Walking across: aim for the side the player is not on.
    if (kind == K_GOAL_CROSS_LINE && K_Map_GetPlayer(&player)
     && (player.x - middle.x) * nx + (player.y - middle.y) * ny > 0.0)
    {
        nx = -nx;
        ny = -ny;
    }

    for (a = 0; a < num_alongs && kind == K_GOAL_CROSS_LINE; ++a)
    {
        k_vec2_t at = { line->v1.x + (line->v2.x - line->v1.x) * alongs[a],
                        line->v1.y + (line->v2.y - line->v1.y) * alongs[a] };

        for (i = 0; i < arrlen(cross_distances); ++i)
        {
            k_vec2_t spot = { at.x + nx * cross_distances[i], at.y + ny * cross_distances[i] };

            if (K_Map_CellAt(spot.x, spot.y) >= 0
             && K_Graph_Clearance(spot, STAND_CLEARANCE) >= STAND_CLEARANCE)
            {
                *x = spot.x;
                *y = spot.y;
                return 1;
            }
        }
    }

    for (a = 0; a < num_alongs; ++a)
    {
        k_vec2_t at = { line->v1.x + (line->v2.x - line->v1.x) * alongs[a],
                        line->v1.y + (line->v2.y - line->v1.y) * alongs[a] };

        for (i = 0; i < arrlen(distances); ++i)
        {
            *x = at.x + nx * distances[i];
            *y = at.y + ny * distances[i];

            if (K_Map_CellAt(*x, *y) < 0)
            {
                continue;
            }

            if (ClearOfBlocks(*x, *y))
            {
                return 1;
            }

            if (!on_floor)
            {
                on_floor = true;
                floor_x = *x;
                floor_y = *y;
            }
        }
    }

    *x = floor_x;
    *y = floor_y;
    return on_floor;
}

int K_Nav_SetLineGoal(int line, k_goalkind_t kind)
{
    double x, y;

    if (!K_Nav_LineGoalPoint(line, kind, &x, &y))
    {
        have_goal = false;
        memset(&status, 0, sizeof(status));
        M_snprintf(status.problem, sizeof(status.problem),
                   "no floor to stand on at line %d", line);
        status.state = K_NAV_FAILED;
        return 0;
    }

    return StartGoal(x, y, kind, line);
}

void K_Nav_Clear(void)
{
    have_goal = false;
    cross_first = false;
    dry_point = -1;
    memset(&status, 0, sizeof(status));
    status.state = K_NAV_IDLE;
    status.goal_line = -1;
    EndAction();
}

const k_path_t *K_Nav_GetPath(void)
{
    return have_goal ? &path : NULL;
}

// As SteerAlongRoute: the leg into a careful point brakes for it, the leg
// out of it is steady.
int K_Nav_CarefulLeg(int leg)
{
    return have_goal && leg >= 0 && leg < path.num_points - 1
        && (careful[leg] || careful[leg + 1]);
}

int K_Nav_CanPause(void)
{
    return status.state != K_NAV_DRIVING
        || ((action.kind == K_ACTION_NONE || action.phase == PHASE_APPROACH)
            && jump_phase == JUMP_NONE && !OnJump(status.target));
}

void K_Nav_Pause(int pause)
{
    k_pose_t pose;
    k_vec2_t at;

    if (paused == (pause != 0))
    {
        return;
    }

    paused = pause != 0;

    if (!paused && status.state == K_NAV_DRIVING && K_Map_GetPlayer(&pose))
    {
        at.x = pose.x;
        at.y = pose.y;
        last_at = at;
        last_floor = pose.floor;
        Replan(at, "back on the way after the fight");
    }
}

const k_navstatus_t *K_Nav_GetStatus(void)
{
    return &status;
}

k_navtuning_t *K_Nav_Tuning(void)
{
    return &tuning;
}
