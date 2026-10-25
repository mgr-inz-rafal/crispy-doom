//
// DESCRIPTION:
//	Karp navigation: follows the route to the goal.
//
//	Think, once per game tic (K_Nav_Tick): which route cell the player is
//	in and which waypoint to drive to next, arrival, and replanning when
//	the player leaves the route or gets stuck.
//
//	Act, once per command (K_Nav_BuildCommand): a command is built a tic
//	or two before it runs, so the controller first predicts where the
//	player will be by then, replaying the commands still waiting to run.
//	It then picks the velocity it wants (full speed, slowing down for
//	corners and the goal) and thrusts the difference between that and the
//	current momentum, split into forward and strafe. Movement therefore
//	does not depend on which way the player faces, and pushing against
//	the momentum brakes.
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

static k_path_t path;
static boolean have_goal;
static k_vec2_t goal;
static k_navstatus_t status;
static k_navtuning_t tuning = { 16.7, 1.5, 24.0, 11.25 };

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
// Controller
//

// The speed to take a waypoint at, from how sharply the route turns there.
static double CornerSpeed(int k)
{
    k_vec2_t a = path.points[k - 1], b = path.points[k], c = path.points[k + 1];
    double in = Distance(a, b), out = Distance(b, c);
    double turn_cos = 1.0;

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

        if (k < last)
        {
            along += Distance(path.points[k], path.points[k + 1]);
        }
    }

    return limit;
}

// Turns toward the desired velocity (desired_x, desired_y), and thrusts
// whatever turns the momentum into it.
static void Steer(const body_t *body, double desired_x, double desired_y,
                  k_command_t *command)
{
    double max_turn = tuning.max_turn * K_PI / 180.0;
    double turn = 0.0;
    double angle, thrust_x, thrust_y, forward, side, scale;

    // Facing is cosmetic for now: strafing moves the player just as well.
    if (desired_x * desired_x + desired_y * desired_y > 0.25)
    {
        turn = atan2(desired_y, desired_x) - body->angle;
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
        int target = status.target;
        int last = path.num_points - 1;
        k_vec2_t at = { body.x, body.y };
        double distance, speed;

        // The waypoint may be reached during the tics still waiting to run.
        if (target < last && Distance(at, path.points[target]) < tuning.advance_radius)
        {
            ++target;
        }

        distance = Distance(at, path.points[target]);
        speed = SpeedLimit(&body, target);

        // Never past the goal within one tic.
        if (target == last)
        {
            speed = SDL_min(speed, distance);
        }

        if (distance > 1e-6)
        {
            Steer(&body, (path.points[target].x - at.x) / distance * speed,
                  (path.points[target].y - at.y) / distance * speed, command);
        }
        else
        {
            Steer(&body, 0.0, 0.0, command);
        }
    }

    Record(tic, command, &body);
}

//
// Following the route
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

static void Fail(const char *problem)
{
    status.state = K_NAV_FAILED;
    M_StringCopy(status.problem, problem, sizeof(status.problem));
}

static void StartRoute(void)
{
    status.target = 1;
    status.route_cell = 0;
    status.remaining = path.length;
    history_count = 0;
    history_next = 0;
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
    int last = path.num_points - 1;
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

    while (status.target < last
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

    if (!Progress(at))
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

    if (Stuck(at))
    {
        stuck_at = at;

        if (++status.stuck > MAX_STUCK)
        {
            M_snprintf(text, sizeof(text), "stuck at (%.0f,%.0f)", at.x, at.y);
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
