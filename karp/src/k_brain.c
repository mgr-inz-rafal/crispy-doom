//
// DESCRIPTION:
//	Karp priority system: a stack of goals. The top one is worked on.
//
//	Under -autoplay every level starts with one goal, GO TO EXIT. A goal
//	is a condition (out of the level, holding a blue key, sector 45 moved)
//	and the places it can be met: its candidates (every exit line, every
//	blue key lying in the level, every line that moves sector 45). Each
//	time a goal is planned, all its candidates are scored by route and the
//	player goes for the best one it can reach. If it can reach none, the
//	candidate whose route is blocked by the most fixable crossings wins,
//	and the fix for the first of them goes on the stack, in front: GET KEY
//	for a locked door, USE TRIGGER for a sector a switch or walk-over line
//	moves. When that is done it comes off, and the goal below is planned
//	again from wherever the player is: the world has changed.
//
//	A goal is done when its condition holds, however that came about. A
//	candidate that fails is not chosen again; a goal whose candidates all
//	fail fails for the rest of the level, is never pushed again, and the
//	goal below looks for another way. A click on the console's map pushes
//	a GOTO on top; when it ends, the rest resumes.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"
#include "r_state.h"

#include "karp.h"
#include "k_brain.h"
#include "k_log.h"
#include "k_nav.h"
#include "k_specials.h"

#define MAX_GOALS 8
#define MAX_FAILED 32
#define MAX_CANDIDATES 64
#define MAX_TRIED 16

// What a blocked crossing adds to a route: one the brain can get past (a
// key lies in the level, some line moves the sector) less than one it
// cannot, so routes go through the fixable ones.
#define FIXABLE_COST 1.0e6
#define UNFIXABLE_COST 1.0e8

// A sector moved by a line: the time it gets to start moving after a
// walk-over line is crossed, and to stop again (perpetual lifts never
// do). After MAX_TRIGGERS times in a level, a line is not worked again:
// e.g. the switch of a door that closes before the player gets there.
// Per line, not per sector: Doom II MAP21 raises a staircase 16 units a
// press, by four switches in a row.
#define START_TICS (2 * TICRATE)
#define MOVE_TICS (15 * TICRATE)
#define MAX_TRIGGERS 3

// No way to any candidate while sectors are still moving (stairs being
// built, a floor going down): plan again after RETRY_TICS, for up to
// MAX_WAIT_TICS in all before the goal fails.
#define RETRY_TICS TICRATE
#define MAX_WAIT_TICS (15 * TICRATE)

// A key is picked up when the player's box touches the key's (radius 20):
// within 36 on each axis. Where the player cannot stand at the key, it
// goes to the nearest point within PICKUP_REACH on each axis (some margin)
// that is on floor and STAND_CLEARANCE from anything that stops it, its
// radius plus one. Searched in steps of PICKUP_STEP.
#define PICKUP_REACH 28.0
#define PICKUP_STEP 4.0
#define STAND_CLEARANCE 17.0

typedef enum
{
    GOAL_GOTO,              // a point (a click)
    GOAL_EXIT,              // any exit line
    GOAL_KEY,               // any key of a colour
    GOAL_TRIGGER,           // any line that moves a sector
} goal_type_t;

// A place a goal can be met, and what to do there.
typedef struct
{
    k_vec2_t at;
    k_goalkind_t kind;
    int line;               // the line to work, -1 for a key or a point
    char what[48];          // "exit switch at line 162", "blue keycard"...
} candidate_t;

typedef struct
{
    goal_type_t type;
    int param;              // GOAL_KEY: the key; GOAL_TRIGGER: the sector
    k_vec2_t point;         // GOAL_GOTO

    // Not planned until a candidate is chosen; planned again when it
    // fails, or a goal in front of this one ends.
    boolean planned;
    candidate_t chosen;
    char note[K_LOG_LINE_LEN];  // what it is doing or waiting for

    k_vec2_t tried[MAX_TRIED];  // candidates that failed
    int num_tried;

    // GOAL_TRIGGER: the sector's heights when its line was gone for.
    double floor, ceiling;
    boolean used;           // a walk-over line was crossed
    boolean moving;
    int wait;

    // GOAL_TRIGGER: the blocked crossing it was pushed for (crossed out of
    // cell from), -1 if none. Once that opens, even if only after a wait
    // (a door opening, a lift on its way down), the goal is done: the
    // player goes on and waits there, not at the switch.
    int portal, from;

    // Found no way while sectors were moving: not planned again before
    // leveltime retry_tic; waited is how long it has waited so far.
    int retry_tic;
    int waited;
} goal_t;

typedef struct
{
    goal_type_t type;
    int param;
    char text[K_LOG_LINE_LEN];
} failed_t;

static goal_t goals[MAX_GOALS];
static int depth;           // goals[depth - 1] is worked on
static failed_t failed[MAX_FAILED];
static int num_failed;
static boolean exit_wanted; // GO TO EXIT goes on an empty list

static int keys_lying[K_NUM_KEYS];  // refreshed when planning
static unsigned char *line_uses;    // per line, as a trigger goal's choice
static k_path_t scratch;

static char outcome[K_LOG_LINE_LEN];    // how the list last emptied
static char status[K_LOG_LINE_LEN * 2];

//
// Names
//

static void GoalName(const goal_t *goal, char *buf, int len)
{
    switch (goal->type)
    {
        case GOAL_EXIT:
            M_StringCopy(buf, "GO TO EXIT", len);
            break;

        case GOAL_KEY:
            M_snprintf(buf, len, "GET %s KEY", goal->param == K_KEY_BLUE ? "BLUE"
                       : goal->param == K_KEY_YELLOW ? "YELLOW" : "RED");
            break;

        // The line's tag: a stair builder's steps after the first are not
        // tagged.
        case GOAL_TRIGGER:
            if (goal->planned && goal->chosen.kind == K_GOAL_USE_LINE)
            {
                M_snprintf(buf, len, "USE SWITCH line %d (tag %d)", goal->chosen.line,
                           lines[goal->chosen.line].tag);
            }
            else if (goal->planned && goal->chosen.kind == K_GOAL_SHOOT_LINE)
            {
                M_snprintf(buf, len, "SHOOT SWITCH line %d (tag %d)", goal->chosen.line,
                           lines[goal->chosen.line].tag);
            }
            else if (goal->planned)
            {
                M_snprintf(buf, len, "CROSS LINE %d (tag %d)", goal->chosen.line,
                           lines[goal->chosen.line].tag);
            }
            else
            {
                M_snprintf(buf, len, "USE TRIGGER for sector %d (tag %d)", goal->param,
                           sectors[goal->param].tag);
            }
            break;

        default:
            M_snprintf(buf, len, "GO TO (%.0f, %.0f)", goal->point.x, goal->point.y);
            break;
    }
}

//
// The stack
//

static boolean IsFailed(goal_type_t type, int param)
{
    int i;

    for (i = 0; i < num_failed; ++i)
    {
        if (failed[i].type == type && failed[i].param == param)
        {
            return true;
        }
    }

    return false;
}

static boolean OnStack(goal_type_t type, int param)
{
    int i;

    for (i = 0; i < depth; ++i)
    {
        if (goals[i].type == type && goals[i].param == param)
        {
            return true;
        }
    }

    return false;
}

// Whether a goal may go on the stack: not one that failed, not one already
// there (it would wait for itself), and not too deep.
static boolean Allowed(goal_type_t type, int param)
{
    return depth < MAX_GOALS && !IsFailed(type, param) && !OnStack(type, param);
}

// Whether a line can still be worked: it has its special (one-shot lines
// lose it once used) and has not been worked MAX_TRIGGERS times.
static boolean LineWorks(int line)
{
    return K_Specials_MoverTrigger(K_Specials_LineSpecial(line)) != K_TRIGGER_NONE
        && (line_uses == NULL || line_uses[line] < MAX_TRIGGERS);
}

static goal_t *Push(goal_type_t type, int param)
{
    goal_t *goal = &goals[depth++];

    memset(goal, 0, sizeof(*goal));
    goal->type = type;
    goal->param = param;
    goal->chosen.line = -1;
    goal->portal = -1;
    goal->from = -1;
    return goal;
}

// The goal below the top is planned again: the world has changed. A click
// that ends on an empty list lets GO TO EXIT start over, failures
// forgotten: the player may stand somewhere that changes things.
static void Pop(void)
{
    --depth;

    if (depth > 0)
    {
        goals[depth - 1].planned = false;
    }
    else if (goals[0].type == GOAL_GOTO)
    {
        exit_wanted = K_AutoplayEnabled();
        num_failed = 0;
    }

    K_Nav_Clear();
}

static void DoneGoal(const char *how)
{
    char name[64];

    GoalName(&goals[depth - 1], name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s done - %s", name, how);

    if (depth == 1)
    {
        M_snprintf(outcome, sizeof(outcome), "%s done - %s", name, how);
    }

    Pop();
}

static void FailGoal(const char *why)
{
    goal_t *goal = &goals[depth - 1];
    char name[64];

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s failed - %s", name, why);

    if (num_failed < MAX_FAILED)
    {
        failed[num_failed].type = goal->type;
        failed[num_failed].param = goal->param;
        M_snprintf(failed[num_failed].text, sizeof(failed[num_failed].text),
                   "%s - %s", name, why);
        ++num_failed;
    }

    if (depth == 1)
    {
        M_snprintf(outcome, sizeof(outcome), "%s failed - %s", name, why);
    }

    // Not added again this level; a click that ends brings it back.
    if (goal->type == GOAL_EXIT)
    {
        exit_wanted = false;
    }

    Pop();
}

// The chosen candidate did not work out: plan again without it.
static void CandidateFailed(goal_t *goal, const char *why)
{
    char name[64];

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: the %s did not work out - %s", name,
          goal->chosen.what, why);

    if (goal->num_tried < MAX_TRIED)
    {
        goal->tried[goal->num_tried++] = goal->chosen.at;
    }

    goal->planned = false;
    K_Nav_Clear();
}

//
// Candidates
//

static boolean WasTried(const goal_t *goal, k_vec2_t at)
{
    int i;

    for (i = 0; i < goal->num_tried; ++i)
    {
        if (fabs(goal->tried[i].x - at.x) < 1.0 && fabs(goal->tried[i].y - at.y) < 1.0)
        {
            return true;
        }
    }

    return false;
}

// Where to go to pick up a key lying at `at`: there, if the player can
// stand there, else the nearest spot it can stand at that still touches
// the key. A key can lie on an impassable line, to be picked up from one
// side only (Doom II MAP05).
static k_vec2_t PickupSpot(k_vec2_t at)
{
    int steps = (int) (PICKUP_REACH / PICKUP_STEP);
    k_vec2_t best = at;
    int best_distance = -1;
    int i, j;

    if (K_Graph_Clearance(at, STAND_CLEARANCE) >= STAND_CLEARANCE)
    {
        return at;
    }

    for (i = -steps; i <= steps; ++i)
    {
        for (j = -steps; j <= steps; ++j)
        {
            k_vec2_t spot = { at.x + i * PICKUP_STEP, at.y + j * PICKUP_STEP };
            int distance = i * i + j * j;

            if ((best_distance < 0 || distance < best_distance)
             && K_Graph_Clearance(spot, STAND_CLEARANCE) >= STAND_CLEARANCE)
            {
                best = spot;
                best_distance = distance;
            }
        }
    }

    return best;
}

static int LineCandidate(int line, k_trigger_t trigger, const char *what,
                         candidate_t *candidate)
{
    candidate->kind = trigger == K_TRIGGER_USE ? K_GOAL_USE_LINE
                    : trigger == K_TRIGGER_SHOOT ? K_GOAL_SHOOT_LINE : K_GOAL_CROSS_LINE;
    candidate->line = line;
    M_snprintf(candidate->what, sizeof(candidate->what), "%s at line %d", what, line);
    return K_Nav_LineGoalPoint(line, candidate->kind, &candidate->at.x,
                               &candidate->at.y);
}

static int Candidates(const goal_t *goal, candidate_t *out, int max)
{
    const k_map_t *map = K_Map_Get();
    int count = 0;
    int i;

    switch (goal->type)
    {
        case GOAL_EXIT:
            for (i = 0; i < map->num_lines && count < max; ++i)
            {
                int secret;
                k_trigger_t trigger = K_Specials_ExitTrigger(map->lines[i].special, &secret);

                if (trigger != K_TRIGGER_NONE
                 && LineCandidate(i, trigger, trigger == K_TRIGGER_USE
                                  ? (secret ? "secret exit switch" : "exit switch")
                                  : (secret ? "secret exit line" : "exit line"),
                                  &out[count]))
                {
                    ++count;
                }
            }
            break;

        case GOAL_KEY:
            {
                k_keything_t keys[MAX_CANDIDATES];
                int found = SDL_min(K_Specials_FindKeys(goal->param, keys, max), max);

                for (i = 0; i < found; ++i)
                {
                    out[count].at = PickupSpot(keys[i].at);
                    out[count].kind = K_GOAL_POINT;
                    out[count].line = -1;
                    M_snprintf(out[count].what, sizeof(out[count].what), "%s %s at "
                               "(%.0f,%.0f)", K_Specials_KeyName(goal->param),
                               keys[i].skull ? "skull key" : "keycard",
                               keys[i].at.x, keys[i].at.y);
                    ++count;
                }
            }
            break;

        case GOAL_TRIGGER:
            for (i = 0; i < K_Specials_TriggerCount(goal->param) && count < max; ++i)
            {
                int line = K_Specials_TriggerLine(goal->param, i);
                k_trigger_t trigger = K_Specials_MoverTrigger(K_Specials_LineSpecial(line));

                if (LineWorks(line)
                 && LineCandidate(line, trigger, trigger == K_TRIGGER_USE ? "switch"
                                  : trigger == K_TRIGGER_SHOOT ? "gun switch"
                                  : "walk-over line", &out[count]))
                {
                    ++count;
                }
            }
            break;

        default:
            out[0].at = goal->point;
            out[0].kind = K_GOAL_POINT;
            out[0].line = -1;
            M_snprintf(out[0].what, sizeof(out[0].what), "point (%.0f,%.0f)",
                       goal->point.x, goal->point.y);
            count = 1;
            break;
    }

    return count;
}

//
// Getting past blocked crossings
//

// Whether some line can still move the sector (LineWorks).
static boolean LiveTrigger(int sector)
{
    int i;

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        if (LineWorks(K_Specials_TriggerLine(sector, i)))
        {
            return true;
        }
    }

    return false;
}

// What would get the player past a blocked crossing, if anything: the key
// for a locked door; for a step too high or too little headroom, a line
// that moves the sector beyond, or the one the player is in; for a gap too
// narrow, a line that moves one of the sectors that narrow it (bars that
// lower), or else either side's (a lift the player rides down to where the
// way on has headroom, Doom II MAP05).
static boolean FixFor(int portal, int from, k_crossing_t crossing, goal_type_t *type,
                      int *param)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *p = &K_Graph_Get()->portals[portal];
    int to = p->cell[0] == from ? p->cell[1] : p->cell[0];
    int from_sector = map->subsectors[map->cells[from].subsector].sector;
    int to_sector = map->subsectors[map->cells[to].subsector].sector;
    int sectors_[8];
    int count = 2;
    int i;

    if (crossing == K_CROSS_LOCKED)
    {
        int key = K_Graph_LockKey(portal);

        if (key >= 0 && keys_lying[key] > 0 && Allowed(GOAL_KEY, key))
        {
            *type = GOAL_KEY;
            *param = key;
            return true;
        }

        return false;
    }

    if (crossing == K_CROSS_NARROW)
    {
        count = K_Graph_NarrowSectors(portal, from, sectors_, arrlen(sectors_) - 2);
        sectors_[count++] = from_sector;
        sectors_[count++] = to_sector;
    }
    else if (crossing == K_CROSS_STEP || crossing == K_CROSS_HEADROOM)
    {
        sectors_[0] = to_sector;
        sectors_[1] = from_sector;
    }
    else
    {
        return false;
    }

    for (i = 0; i < count; ++i)
    {
        if (sectors_[i] >= 0 && LiveTrigger(sectors_[i])
         && Allowed(GOAL_TRIGGER, sectors_[i]))
        {
            *type = GOAL_TRIGGER;
            *param = sectors_[i];
            return true;
        }
    }

    return false;
}

// The fix for a blocked route: for its first blocked crossing, or none.
// But a key comes first, wherever its door is on the route: keys stay,
// while what a switch does may not last until the player gets there (a
// lift goes back up while it fetches the key, Doom II MAP12). Sets
// key_door to that door's portal, else -1.
static boolean RouteFix(const k_path_t *path, goal_type_t *type, int *param,
                        int *key_door)
{
    goal_type_t key_type;
    int key_param;
    int i;

    *key_door = -1;

    // A gap inside a cell is no crossing's: nothing found fixes it.
    if (path->blocked_pinch
     || !FixFor(path->blocked_portal, path->blocked_from, path->blocked_by, type, param))
    {
        return false;
    }

    for (i = 0; i < path->num_cells - 1 && *type != GOAL_KEY; ++i)
    {
        if (K_Graph_CanCross(path->portals[i], path->cells[i]) == K_CROSS_LOCKED
         && FixFor(path->portals[i], path->cells[i], K_CROSS_LOCKED, &key_type, &key_param))
        {
            *type = key_type;
            *param = key_param;
            *key_door = path->portals[i];
        }
    }

    return true;
}

// Set as the pathfinder's block cost, for every route planned (nav's
// replans too).
static double BlockCost(int portal, int from, k_crossing_t crossing)
{
    goal_type_t type;
    int param;

    return FixFor(portal, from, crossing, &type, &param) ? FIXABLE_COST : UNFIXABLE_COST;
}

static void CountKeys(void)
{
    int key;

    for (key = 0; key < K_NUM_KEYS; ++key)
    {
        keys_lying[key] = K_Specials_FindKeys(key, NULL, 0);
    }
}

//
// Planning
//

static void Go(goal_t *goal, const candidate_t *candidate, double length,
               int reachable, int total)
{
    char name[64];
    int ok;

    goal->chosen = *candidate;
    goal->planned = true;
    goal->used = false;
    goal->moving = false;
    goal->wait = 0;
    goal->waited = 0;

    if (goal->type == GOAL_TRIGGER)
    {
        K_Map_SectorHeights(goal->param, &goal->floor, &goal->ceiling);

        if (line_uses != NULL && candidate->line >= 0)
        {
            ++line_uses[candidate->line];
        }
    }

    GoalName(goal, name, sizeof(name));
    M_snprintf(goal->note, sizeof(goal->note), "%s, %.0f units (%d of %d reachable)",
               candidate->what, length, reachable, total);
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: going for the %s", name, goal->note);

    ok = candidate->kind == K_GOAL_POINT
       ? K_Nav_SetGoal(candidate->at.x, candidate->at.y)
       : K_Nav_SetLineGoal(candidate->line, candidate->kind);

    if (!ok)
    {
        CandidateFailed(goal, K_Nav_GetStatus()->problem);
    }
}

// Why each candidate was given up on, while planning a goal: logged one
// by one when it fails (the failure itself names only the last).
#define MAX_REASONS 8

static char reasons[MAX_REASONS][K_LOG_LINE_LEN];
static int num_reasons;

static void KeepReason(const char *reason)
{
    if (num_reasons < MAX_REASONS)
    {
        M_StringCopy(reasons[num_reasons++], reason, K_LOG_LINE_LEN);
    }
}

static void LogReasons(const char *name)
{
    int i;

    for (i = 0; i < num_reasons && num_reasons > 1; ++i)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s", name, reasons[i]);
    }
}

static void PlanGoal(goal_t *goal)
{
    static candidate_t candidates[MAX_CANDIDATES];
    int count = Candidates(goal, candidates, MAX_CANDIDATES);
    int best_open = -1, best_blocked = -1, reachable = 0;
    int waitable = 0;   // candidates with no way now, which a move may open
    int moving;
    double open_length = 0.0, blocked_score = 0.0;
    goal_type_t fix_type = GOAL_GOTO;
    int fix_param = 0, fix_portal = -1, fix_from = -1;
    char reason[K_LOG_LINE_LEN], blocker[K_LOG_LINE_LEN] = "";
    char name[64];
    k_pose_t player;
    k_vec2_t start;
    int i;

    if (!K_Map_GetPlayer(&player))
    {
        return;
    }

    start.x = player.x;
    start.y = player.y;
    CountKeys();
    GoalName(goal, name, sizeof(name));
    num_reasons = 0;

    M_snprintf(reason, sizeof(reason), "%s",
               goal->type == GOAL_EXIT ? "the map has no exit line"
             : goal->type == GOAL_KEY ? "no such key lies in the level"
             : goal->type == GOAL_TRIGGER ? "no line moves the sector any more"
             : "no way there");

    for (i = 0; i < count; ++i)
    {
        char text[K_LOG_LINE_LEN];
        goal_type_t type;
        int param, key_door;

        if (WasTried(goal, candidates[i].at))
        {
            M_snprintf(reason, sizeof(reason), "the %s did not work out",
                       candidates[i].what);
            KeepReason(reason);
            continue;
        }

        K_Path_Find(&scratch, start, player.cell, candidates[i].at);
        K_Path_Describe(&scratch, text, sizeof(text));

        // A switch that needs a key (red bars, say): nothing happens until
        // the player has it.
        if (candidates[i].line >= 0)
        {
            int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));

            if (key >= 0 && !K_Specials_HasKey(key))
            {
                if (scratch.result != K_PATH_FOUND && scratch.result != K_PATH_BLOCKED)
                {
                    M_snprintf(reason, sizeof(reason), "no way to the %s: %s",
                               candidates[i].what, text);
                    KeepReason(reason);
                    ++waitable;
                }
                else if (keys_lying[key] > 0 && Allowed(GOAL_KEY, key))
                {
                    double score = (scratch.blocked_count + 1) * FIXABLE_COST
                                 + scratch.length;

                    if (best_blocked < 0 || score < blocked_score)
                    {
                        best_blocked = i;
                        blocked_score = score;
                        fix_type = GOAL_KEY;
                        fix_param = key;
                        M_snprintf(blocker, sizeof(blocker), "the %s needs the %s key",
                                   candidates[i].what, K_Specials_KeyName(key));
                    }
                }
                else
                {
                    M_snprintf(reason, sizeof(reason), "the %s needs the %s key, and "
                               "there is none to get", candidates[i].what,
                               K_Specials_KeyName(key));
                    KeepReason(reason);
                }

                continue;
            }
        }

        if (scratch.result == K_PATH_FOUND)
        {
            ++reachable;

            if (best_open < 0 || scratch.length < open_length)
            {
                best_open = i;
                open_length = scratch.length;
            }
        }
        else if (scratch.result == K_PATH_BLOCKED
              && RouteFix(&scratch, &type, &param, &key_door))
        {
            double score = scratch.blocked_count * FIXABLE_COST + scratch.length;

            if (best_blocked < 0 || score < blocked_score)
            {
                best_blocked = i;
                blocked_score = score;
                fix_type = type;
                fix_param = param;
                fix_portal = scratch.blocked_portal;
                fix_from = scratch.blocked_from;

                if (key_door >= 0)
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by %s, and "
                               "further on by a door that needs the %s key - the key "
                               "first", candidates[i].what, text,
                               K_Graph_LockName(key_door));
                }
                else
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by %s",
                               candidates[i].what, text);
                }
            }
        }
        else
        {
            M_snprintf(reason, sizeof(reason), "no way to the %s: %s",
                       candidates[i].what, text);
            KeepReason(reason);
            ++waitable;
        }
    }

    if (best_open >= 0)
    {
        Go(goal, &candidates[best_open], open_length, reachable, count);
    }
    else if (best_blocked >= 0)
    {
        goal_t *fix;
        char fix_name[64];

        M_StringCopy(goal->note, blocker, sizeof(goal->note));
        fix = Push(fix_type, fix_param);

        if (fix_type == GOAL_TRIGGER)
        {
            fix->portal = fix_portal;
            fix->from = fix_from;
        }

        GoalName(fix, fix_name, sizeof(fix_name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s - added %s", name, blocker, fix_name);
    }
    else if (waitable > 0 && (moving = K_Graph_MovingSector()) >= 0
          && goal->waited < MAX_WAIT_TICS)
    {
        // The way may still open (stairs being built, a floor going down,
        // Doom II MAP15 and MAP13): stand still and plan again shortly.
        // Not for candidates already tried: they stay given up on, so
        // nothing that moves brings them back (MAP17 stood 15 s for a
        // walk-over line it could not get to).
        if (goal->waited == 0)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: no way yet (%s), but sector %d is "
                  "moving - waiting", name, reason, moving);
        }

        M_snprintf(goal->note, sizeof(goal->note), "waiting while sectors move - %s",
                   reason);
        goal->retry_tic = leveltime + RETRY_TICS;
        goal->waited += RETRY_TICS;
        K_Nav_Clear();
    }
    else
    {
        LogReasons(name);
        FailGoal(reason);
    }
}

// A trigger goal is done once the crossing it was pushed for opens: a door
// opening or a lift coming down is a crossing to wait at, so the player
// sets off for it at once, as a human would. Judged before the goal is
// planned too: the way may have opened while a goal in front of it was
// worked on (Doom II MAP24: the red key platform rising with the player),
// and planning would send the player off for nothing and count a use of
// the line. Returns true if the goal is done.
static boolean DoneIfOpen(const goal_t *goal)
{
    k_crossing_t crossing;
    char how[96];

    if (goal->type != GOAL_TRIGGER || goal->portal < 0)
    {
        return false;
    }

    crossing = K_Graph_CanCross(goal->portal, goal->from);

    if (!K_Graph_Passable(crossing))
    {
        return false;
    }

    M_snprintf(how, sizeof(how), "the way on is open, or opening (%s)",
               K_Graph_CrossingName(crossing));
    DoneGoal(how);
    return true;
}

// A sector moved by a line: once the line is used, wait for the sector to
// start moving, then to stop (unless the way opens first: DoneIfOpen).
// Returns true while it handles the goal.
static boolean UpdateTrigger(goal_t *goal)
{
    double floor, ceiling;
    k_mover_t mover = K_Graph_SectorMover(goal->param);
    char how[96];

    K_Map_SectorHeights(goal->param, &floor, &ceiling);

    if (!goal->moving)
    {
        if (floor != goal->floor || ceiling != goal->ceiling || mover != K_MOVER_NONE)
        {
            goal->moving = true;
            goal->wait = 0;
            M_snprintf(goal->note, sizeof(goal->note), "sector %d is moving",
                       goal->param);
            K_Nav_Clear();
            return true;
        }

        if (goal->used && ++goal->wait > START_TICS)
        {
            CandidateFailed(goal, "crossing it moved nothing");
            return true;
        }

        return false;
    }

    if (mover == K_MOVER_NONE || mover == K_MOVER_WAITING || ++goal->wait > MOVE_TICS)
    {
        M_snprintf(how, sizeof(how), "sector %d moved (floor %.0f, ceiling %.0f)",
                   goal->param, floor, ceiling);
        DoneGoal(how);
    }

    return true;
}

//
// Interface
//

void K_Brain_Reset(void)
{
    if (depth > 0)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%d goals dropped - new level", depth);
    }

    depth = 0;
    num_failed = 0;
    outcome[0] = '\0';
    exit_wanted = K_AutoplayEnabled();

    free(line_uses);
    line_uses = calloc(SDL_max(numlines, 1), 1);

    K_Path_SetBlockCost(BlockCost);
}

void K_Brain_RequestGoto(double x, double y)
{
    k_pose_t player;
    goal_t *goal;

    if (!K_Map_GetPlayer(&player))
    {
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "click at (%.0f,%.0f) ignored - not in "
              "a level", x, y);
        return;
    }

    if (K_Map_CellAt(x, y) < 0)
    {
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "click at (%.0f,%.0f) ignored - not on "
              "any floor", x, y);
        return;
    }

    // A new click replaces one still being gone to.
    if (depth > 0 && goals[depth - 1].type == GOAL_GOTO)
    {
        Pop();
    }

    if (depth >= MAX_GOALS)
    {
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "click at (%.0f,%.0f) ignored - the goal "
              "list is full", x, y);
        return;
    }

    goal = Push(GOAL_GOTO, -1);
    goal->point.x = x;
    goal->point.y = y;
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added GO TO (%.0f, %.0f) - user click", x, y);

    // Planned now, so the route shows even when nothing ticks (viewer
    // mode).
    PlanGoal(goal);
}

void K_Brain_Tick(void)
{
    goal_t *goal;

    if (depth == 0 && exit_wanted)
    {
        Push(GOAL_EXIT, 0);
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "added GO TO EXIT");
    }

    if (depth == 0)
    {
        return;
    }

    goal = &goals[depth - 1];

    if (goal->type == GOAL_KEY && K_Specials_HasKey(goal->param))
    {
        DoneGoal("picked it up");
        return;
    }

    if (DoneIfOpen(goal))
    {
        return;
    }

    if (!goal->planned)
    {
        if (leveltime >= goal->retry_tic)
        {
            PlanGoal(goal);
        }

        return;
    }

    if (goal->type == GOAL_TRIGGER && UpdateTrigger(goal))
    {
        return;
    }

    switch (K_Nav_Tick())
    {
        case K_NAV_ARRIVED:
            if (goal->type == GOAL_GOTO)
            {
                DoneGoal("arrived");
            }
            else if (goal->type == GOAL_TRIGGER)
            {
                // A walk-over line: crossed on the way.
                goal->used = true;
                goal->wait = 0;
            }
            else if (goal->type == GOAL_KEY)
            {
                CandidateFailed(goal, "got there, but did not pick it up");
            }
            else
            {
                CandidateFailed(goal, "crossed it, but the level did not end");
            }
            break;

        case K_NAV_FAILED:
            CandidateFailed(goal, K_Nav_GetStatus()->problem);
            break;

        default:
            break;
    }
}

void K_Brain_LevelCompleted(int secret)
{
    char how[96];

    M_snprintf(how, sizeof(how), "level finished in %.1f s through the %s exit",
               (double) leveltime / TICRATE, secret ? "secret" : "normal");

    if (depth > 0 && goals[0].type == GOAL_EXIT)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT done - %s", how);
    }
    else
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s", how);
    }

    depth = 0;
    exit_wanted = false;
}

int K_Brain_GoalCount(void)
{
    return depth;
}

void K_Brain_GoalText(int index, char *buf, int buf_len)
{
    const goal_t *goal = &goals[depth - 1 - index];
    char name[64];

    GoalName(goal, name, sizeof(name));

    if (goal->note[0] != '\0')
    {
        M_snprintf(buf, buf_len, "%s - %s", name, goal->note);
    }
    else
    {
        M_StringCopy(buf, name, buf_len);
    }
}

int K_Brain_FailedCount(void)
{
    return num_failed;
}

const char *K_Brain_FailedText(int index)
{
    return failed[index].text;
}

const char *K_Brain_Status(void)
{
    const char *mode = K_AutoplayEnabled() ? "" : "VIEWER - ";
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    char top[K_LOG_LINE_LEN];

    if (depth > 0)
    {
        K_Brain_GoalText(0, top, sizeof(top));

        if (path != NULL && K_AutoplayEnabled() && nav->state == K_NAV_DRIVING)
        {
            M_snprintf(status, sizeof(status), "%s - %d waypoints left, %.0f units",
                       top, path->num_points - nav->target, nav->remaining);
        }
        else
        {
            M_snprintf(status, sizeof(status), "%s%s", mode, top);
        }
    }
    else if (outcome[0] != '\0')
    {
        M_snprintf(status, sizeof(status), "%sIDLE - %s", mode, outcome);
    }
    else if (!K_AutoplayEnabled())
    {
        M_snprintf(status, sizeof(status), "VIEWER - click the map to plan a "
                   "path; run with -autoplay to let the bot drive");
    }
    else
    {
        M_StringCopy(status, "IDLE", sizeof(status));
    }

    return status;
}
