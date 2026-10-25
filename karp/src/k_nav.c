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

#include <math.h>
#include <string.h>

#include "SDL.h"

#include "d_loop.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"

#include "k_log.h"
#include "k_nav.h"

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

// Commands sent, by tic, for prediction.
#define SENT_SIZE 16

// Doors and lifts. The player stands ACTION_DISTANCE in front of the line
// (its radius is 16) and presses use when the trace would hit the line
// within USE_REACH (the engine's USERANGE is 64). It turns to face it at
// up to ACTION_TURN degrees a tic, and holds the spot while waiting.
#define ACTION_DISTANCE 24.0
#define USE_REACH 60.0
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

typedef struct
{
    double x, y;
    double momx, momy;
    double angle;
    boolean onground;
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

static const char *phase_names[] = { "walking up", "pressing use", "waiting" };

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
    int presses;
    int press_tic;          // the tic of the last press
} action_t;

static k_path_t path;
static boolean have_goal;
static k_vec2_t goal;
static k_navstatus_t status;
static k_navtuning_t tuning = { 16.7, 1.5, 24.0, 11.25 };
static action_t action;

static sent_t sent[SENT_SIZE];

// Positions over the last STUCK_TICS tics.
static k_vec2_t history[STUCK_TICS];
static int history_count;
static int history_next;
static k_vec2_t stuck_at;

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

// One tic of player movement, as P_MovePlayer and P_XYMovement do it,
// without walls.
static void Step(body_t *body, const k_command_t *command)
{
    body->angle += command->turn / ANGLETURN_PER_RADIAN;

    if (body->onground)
    {
        double forward = command->forward * THRUST_SCALE;
        double side = command->side * THRUST_SCALE;

        body->momx += forward * cos(body->angle) + side * sin(body->angle);
        body->momy += forward * sin(body->angle) - side * cos(body->angle);
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

// The position of a cell in the route, or -1. A route visits a cell once.
static int RouteIndex(int cell)
{
    int i;

    for (i = 0; i < path.num_cells; ++i)
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

// The furthest waypoint the target may move to: not past a crossing that
// is not open yet.
static int TargetLimit(void)
{
    int r = status.route_cell;

    if (r < path.num_cells - 1 && CrossingOut(r) != K_CROSS_OPEN)
    {
        return ExitPoint(r);
    }

    return path.num_points - 1;
}

//
// Controller
//

// The speed to take a waypoint at, from how sharply the route turns there.
// 0 in front of a door or lift.
static double CornerSpeed(int k)
{
    k_vec2_t a = path.points[k - 1], b = path.points[k], c = path.points[k + 1];
    double in = Distance(a, b), out = Distance(b, c);
    double turn_cos = 1.0;
    int r = path.leg_cells[k - 1];

    if (path.leg_cells[k] != r && CrossingOut(r) != K_CROSS_OPEN)
    {
        return 0.0;
    }

    if (in > 1e-6 && out > 1e-6)
    {
        turn_cos = ((b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y))
                 / (in * out);
    }

    return tuning.max_speed * SDL_max(MIN_CORNER_SPEED, SDL_min(turn_cos, 1.0));
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

    for (k = target; k <= last && along < LOOKAHEAD; ++k)
    {
        double corner = k == last ? 0.0 : CornerSpeed(k);

        limit = SDL_min(limit, sqrt(corner * corner + 2.0 * tuning.braking * along));

        if (corner == 0.0)
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

// Thrusts whatever turns the momentum into the desired velocity
// (desired_x, desired_y), and turns toward it - or toward the angle face,
// if given.
static void Steer(const body_t *body, double desired_x, double desired_y,
                  const double *face, k_command_t *command)
{
    double max_turn = (face != NULL ? ACTION_TURN : tuning.max_turn) * K_PI / 180.0;
    double turn = 0.0;
    double angle, thrust_x, thrust_y, forward, side, scale;

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
        Steer(body, 0.0, 0.0, face, command);
        return;
    }

    Steer(body, (to.x - at.x) / distance * speed, (to.y - at.y) / distance * speed,
          face, command);
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

    Steer(body, x, y, face, command);
}

static void SteerAlongRoute(const body_t *body, k_command_t *command)
{
    int target = status.target;
    int last = path.num_points - 1;
    k_vec2_t at = { body->x, body->y };
    double distance, speed;

    // The waypoint may be reached during the tics still waiting to run.
    if (target < TargetLimit()
     && Distance(at, path.points[target]) < tuning.advance_radius)
    {
        ++target;
    }

    distance = Distance(at, path.points[target]);
    speed = SpeedLimit(body, target);

    // Never past the goal within one tic.
    if (target == last)
    {
        speed = SDL_min(speed, distance);
    }

    if (distance > 1e-6)
    {
        Steer(body, (path.points[target].x - at.x) / distance * speed,
              (path.points[target].y - at.y) / distance * speed, NULL, command);
    }
    else
    {
        Steer(body, 0.0, 0.0, NULL, command);
    }
}

// Whether use, pressed in the command about to be sent, would work the
// action's line: the engine traces 64 units straight ahead from where the
// player is when the tic starts, after the tic's turn.
static boolean UseReaches(const body_t *body, const k_command_t *command)
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

    return t > 0.0 && t <= USE_REACH && u > 0.05 && u < 0.95;
}

// Pressing use on a door that is opening (or open) closes it, and on a
// busy lift does nothing.
static boolean WorthPressing(void)
{
    k_mover_t mover = K_Graph_SectorMover(action.sector);

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
                    Distance(at, action.spot) < 96.0 ? &action.face : NULL, command);
        return;
    }

    SteerToHold(body, action.spot, &action.face, command);

    // Use is released for a tic between presses, or the engine ignores it.
    if (action.phase == PHASE_AIM && action.kind != K_ACTION_RIDE
     && tic - action.press_tic > 1 && speed < 4.0
     && WorthPressing() && UseReaches(body, command))
    {
        command->use = 1;
        action.phase = PHASE_WAIT;
        action.tics = 0;
        action.press_tic = tic;
        ++action.presses;
    }
}

void K_Nav_BuildCommand(int tic, k_command_t *command)
{
    body_t body;

    memset(command, 0, sizeof(*command));

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

    Record(tic, command, &body);
}

//
// Doors and lifts
//

static void Fail(const char *problem)
{
    status.state = K_NAV_FAILED;
    M_StringCopy(status.problem, problem, sizeof(status.problem));
}

static void ActionText(void)
{
    static const char *kinds[] = { "", "door", "lift", "lift ride" };

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

// The crossing out of route cell r needs a door or lift.
static void StartAction(k_crossing_t crossing, int r)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *portal = &K_Graph_Get()->portals[path.portals[r]];
    int from = path.cells[r], to = path.cells[r + 1];
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
    action.spot.x = middle.x + nx * ACTION_DISTANCE;
    action.spot.y = middle.y + ny * ACTION_DISTANCE;
    action.face = atan2(-ny, -nx);
    action.start_tics = status.tics;
    action.press_tic = -2;

    // A ride watches the lift the player is on; the others what is ahead.
    action.sector = map->subsectors[map->cells[action.kind == K_ACTION_RIDE
                                               ? from : to].subsector].sector;
    action.height = PlaneHeight();

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
            K_Log(K_LOG_NAV, K_LOG_INFO, "calling the lift at line %d (%.0f,%.0f)",
                  action.line, middle.x, middle.y);
            break;

        default:
            K_Log(K_LOG_NAV, K_LOG_INFO, "waiting for the lift (sector %d) to rise",
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
            K_Log(K_LOG_NAV, K_LOG_INFO, "lift up after %.1f s", seconds);
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

static boolean Replan(k_vec2_t at, const char *why);

// Works the door or lift at the crossing ahead, if any. Returns false if
// the route was replaced or failed.
static boolean UpdateAction(k_vec2_t at, double speed)
{
    int r = status.route_cell;
    k_crossing_t crossing;
    k_mover_t mover;
    double height;
    char why[96];

    if (action.kind != K_ACTION_NONE && action.route_cell != r)
    {
        EndAction();
    }

    if (r >= path.num_cells - 1)
    {
        return true;
    }

    crossing = CrossingOut(r);

    if (crossing == K_CROSS_OPEN)
    {
        if (action.kind != K_ACTION_NONE)
        {
            FinishAction();
        }

        return true;
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
    mover = action.kind == K_ACTION_RIDE ? K_MOVER_NONE : K_Graph_SectorMover(action.sector);

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

// Off the route, but one open step from it: e.g. a corner cut through a
// neighbouring cell. Not worth a new route.
static boolean NextToRoute(int cell)
{
    const k_graph_t *graph = K_Graph_Get();
    int i, end = graph->first_link[cell] + graph->link_count[cell];

    for (i = graph->first_link[cell]; i < end; ++i)
    {
        if (RouteIndex(graph->links[i].neighbor) >= 0
         && K_Graph_CanCross(graph->links[i].portal, cell) == K_CROSS_OPEN)
        {
            return true;
        }
    }

    return false;
}

static void StartRoute(void)
{
    status.target = 1;
    status.route_cell = 0;
    status.remaining = path.length;
    history_count = 0;
    history_next = 0;
    EndAction();
}

// A new route from where the player is. Returns false (and fails) if
// there is none.
static boolean Replan(k_vec2_t at, const char *why)
{
    char text[K_LOG_LINE_LEN];
    char problem[sizeof(status.problem)];

    ++status.replans;
    K_Path_Find(&path, at, goal);
    K_Path_Describe(&path, text, sizeof(text));

    if (path.result != K_PATH_FOUND)
    {
        K_Log(K_LOG_NAV, K_LOG_WARN, "%s - no path: %s", why, text);
        M_snprintf(problem, sizeof(problem), "no path: %s", text);
        Fail(problem);
        return false;
    }

    K_Log(K_LOG_NAV, K_LOG_INFO, "%s - replanned: %s", why, text);
    StartRoute();
    return true;
}

// Moves the target along as the player goes. Returns false if the route
// was replaced or failed.
static boolean Progress(k_vec2_t at)
{
    int cell = K_Map_CellAt(at.x, at.y);
    int r = RouteIndex(cell);
    int limit;
    char why[96];

    if (r > status.route_cell)
    {
        // Further along (perhaps having cut a corner): never back.
        status.target = SDL_max(status.target, FirstPointIn(r));
        status.route_cell = r;
    }
    else if (r >= 0 && r < status.route_cell)
    {
        // Pushed back: drive out of this cell again.
        status.target = FirstPointIn(r);
        status.route_cell = r;
    }
    else if (r < 0 && cell >= 0 && !NextToRoute(cell))
    {
        M_snprintf(why, sizeof(why), "off the route in cell %d at (%.0f,%.0f)",
                   cell, at.x, at.y);
        return Replan(at, why);
    }

    limit = TargetLimit();

    while (status.target < limit
        && Distance(at, path.points[status.target]) < tuning.advance_radius)
    {
        ++status.target;
    }

    return true;
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
        remaining += Distance(path.points[k], path.points[k + 1]);
    }

    return remaining;
}

k_navstate_t K_Nav_Tick(void)
{
    k_pose_t pose;
    k_vec2_t at;
    char text[96];

    if (status.state != K_NAV_DRIVING || !K_Map_GetPlayer(&pose))
    {
        return status.state;
    }

    at.x = pose.x;
    at.y = pose.y;
    ++status.tics;
    status.speed = sqrt(pose.momx * pose.momx + pose.momy * pose.momy);
    CheckPrediction(at);

    if (!Progress(at) || !UpdateAction(at, status.speed))
    {
        return status.state;
    }

    if (status.target == path.num_points - 1 && Distance(at, goal) < ARRIVE_RADIUS
     && status.speed < ARRIVE_SPEED)
    {
        have_goal = false;
        status.state = K_NAV_IDLE;
        status.remaining = 0.0;
        return K_NAV_ARRIVED;
    }

    if (status.stuck > 0 && Distance(at, stuck_at) > UNSTUCK_DISTANCE)
    {
        status.stuck = 0;
    }

    // Standing still is the point while working a door or lift; the action
    // has its own time limits.
    if (action.kind != K_ACTION_NONE && action.phase != PHASE_APPROACH)
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

void K_Nav_Reset(void)
{
    int i;

    have_goal = false;
    memset(&status, 0, sizeof(status));
    status.state = K_NAV_IDLE;
    EndAction();

    for (i = 0; i < SENT_SIZE; ++i)
    {
        sent[i].tic = -1;
    }
}

int K_Nav_SetGoal(double x, double y)
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

    K_Path_Find(&path, start, goal);
    have_goal = true;

    memset(&status, 0, sizeof(status));
    status.state = K_NAV_DRIVING;
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

const k_path_t *K_Nav_GetPath(void)
{
    return have_goal ? &path : NULL;
}

const k_navstatus_t *K_Nav_GetStatus(void)
{
    return &status;
}

k_navtuning_t *K_Nav_Tuning(void)
{
    return &tuning;
}
