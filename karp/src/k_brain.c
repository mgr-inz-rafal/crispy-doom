//
// DESCRIPTION:
//	Karp priority system. Under -autoplay every level starts with one
//	priority on the list, GO TO EXIT: the nearest exit line the player can
//	reach (normal or secret), crossed or pressed as its type needs. A
//	click on the console's map is a GOTO that takes over until it ends;
//	then GO TO EXIT resumes.
//

#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"

#include "karp.h"
#include "k_brain.h"
#include "k_log.h"
#include "k_nav.h"

typedef enum
{
    PRIORITY_IDLE,
    PRIORITY_GOTO,
    PRIORITY_EXIT,
} priority_type_t;

// How the last priority ended.
typedef enum
{
    OUTCOME_NONE,
    OUTCOME_DONE,
    OUTCOME_FAILED,
} outcome_t;

static priority_type_t priority;
static double goto_x, goto_y;

// GO TO EXIT is on the list (-autoplay), and the exit it is going to.
static boolean exit_wanted;
static int exit_line;
static const char *exit_name;

static priority_type_t outcome_priority;
static outcome_t outcome;
static int outcome_tics;
static char failure[K_LOG_LINE_LEN];

static char status[K_LOG_LINE_LEN + 64];

// Scratch route for comparing exits.
static k_path_t candidate;

static const char *PriorityName(priority_type_t type)
{
    return type == PRIORITY_EXIT ? "GO TO EXIT" : "GOTO";
}

static void Fail(const char *problem)
{
    M_StringCopy(failure, problem, sizeof(failure));
    outcome_priority = priority;
    outcome = OUTCOME_FAILED;
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "priority %s failed - %s", PriorityName(priority),
          failure);

    // No retrying an exit that just failed; a GOTO that ends, or a new
    // level, brings it back.
    if (priority == PRIORITY_EXIT)
    {
        exit_wanted = false;
    }

    priority = PRIORITY_IDLE;
}

//
// GO TO EXIT
//

// Exit lines (p_switch.c, p_spec.c): switches are pressed, walk-over lines
// crossed.
static boolean ExitLine(int special, k_goalkind_t *kind, const char **name)
{
    switch (special)
    {
        case 11:
            *kind = K_GOAL_USE_LINE;
            *name = "exit switch";
            return true;

        case 51:
            *kind = K_GOAL_USE_LINE;
            *name = "secret exit switch";
            return true;

        case 52:
            *kind = K_GOAL_CROSS_LINE;
            *name = "exit line";
            return true;

        case 124:
            *kind = K_GOAL_CROSS_LINE;
            *name = "secret exit line";
            return true;

        default:
            return false;
    }
}

// Picks the exit with the shortest route and sends nav there.
static void StartExit(void)
{
    const k_map_t *map = K_Map_Get();
    int best_line = -1;
    k_goalkind_t best_kind = K_GOAL_POINT;
    const char *best_name = NULL;
    double best_length = 0.0;
    int exits = 0;
    char reason[K_LOG_LINE_LEN] = "the map has no exit line";
    k_pose_t player;
    int i;

    if (!K_Map_GetPlayer(&player))
    {
        return;
    }

    for (i = 0; i < map->num_lines; ++i)
    {
        k_goalkind_t kind;
        const char *name;
        k_vec2_t start = { player.x, player.y }, goal;

        if (!ExitLine(map->lines[i].special, &kind, &name))
        {
            continue;
        }

        ++exits;

        if (!K_Nav_LineGoalPoint(i, kind, &goal.x, &goal.y))
        {
            M_snprintf(reason, sizeof(reason), "no floor to stand on at the %s "
                       "(line %d)", name, i);
            continue;
        }

        K_Path_Find(&candidate, start, goal);

        if (candidate.result != K_PATH_FOUND)
        {
            char text[K_LOG_LINE_LEN];

            K_Path_Describe(&candidate, text, sizeof(text));
            M_snprintf(reason, sizeof(reason), "no path to the %s (line %d): %s",
                       name, i, text);
            continue;
        }

        if (best_line < 0 || candidate.length < best_length)
        {
            best_line = i;
            best_kind = kind;
            best_name = name;
            best_length = candidate.length;
        }
    }

    priority = PRIORITY_EXIT;
    outcome = OUTCOME_NONE;

    if (best_line < 0)
    {
        Fail(reason);
        return;
    }

    exit_line = best_line;
    exit_name = best_name;

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "new priority GO TO EXIT - %s at line %d, "
          "%.0f units (%d exit lines on the map)", best_name, best_line,
          best_length, exits);

    if (!K_Nav_SetLineGoal(best_line, best_kind))
    {
        Fail(K_Nav_GetStatus()->problem);
    }
}

//
// Interface
//

void K_Brain_Reset(void)
{
    if (priority == PRIORITY_GOTO)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GOTO (%.0f,%.0f) dropped - "
              "new level", goto_x, goto_y);
    }

    priority = PRIORITY_IDLE;
    outcome = OUTCOME_NONE;
    exit_wanted = K_AutoplayEnabled();
}

void K_Brain_RequestGoto(double x, double y)
{
    k_pose_t player;

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

    if (priority == PRIORITY_EXIT)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GO TO EXIT paused - user click");
    }

    priority = PRIORITY_GOTO;
    goto_x = x;
    goto_y = y;
    outcome = OUTCOME_NONE;

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "new priority GOTO (%.0f,%.0f) - user click",
          x, y);

    if (!K_Nav_SetGoal(x, y))
    {
        Fail(K_Nav_GetStatus()->problem);
    }
}

void K_Brain_Tick(void)
{
    if (priority == PRIORITY_IDLE && exit_wanted)
    {
        StartExit();
    }

    if (priority == PRIORITY_IDLE)
    {
        return;
    }

    switch (K_Nav_Tick())
    {
        case K_NAV_ARRIVED:
            if (priority == PRIORITY_EXIT)
            {
                // A walk-over exit ends the level as the player crosses it.
                Fail("crossed the exit line, but the level did not end");
                break;
            }

            outcome_priority = priority;
            outcome = OUTCOME_DONE;
            outcome_tics = K_Nav_GetStatus()->tics;
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GOTO (%.0f,%.0f) done - "
                  "arrived in %.1f s", goto_x, goto_y,
                  (double) outcome_tics / TICRATE);
            priority = PRIORITY_IDLE;
            exit_wanted = K_AutoplayEnabled();
            break;

        case K_NAV_FAILED:
            Fail(K_Nav_GetStatus()->problem);

            if (outcome_priority == PRIORITY_GOTO)
            {
                exit_wanted = K_AutoplayEnabled();
            }
            break;

        default:
            break;
    }
}

void K_Brain_LevelCompleted(int secret)
{
    if (priority == PRIORITY_EXIT)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GO TO EXIT done - level finished "
              "in %.1f s through the %s exit", (double) leveltime / TICRATE,
              secret ? "secret" : "normal");
    }
    else
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "level finished in %.1f s through the %s "
              "exit", (double) leveltime / TICRATE, secret ? "secret" : "normal");
    }

    priority = PRIORITY_IDLE;
}

const char *K_Brain_Status(void)
{
    const char *mode = K_AutoplayEnabled() ? "" : "VIEWER - ";
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    char what[64];

    if (priority == PRIORITY_EXIT)
    {
        M_snprintf(what, sizeof(what), "GO TO EXIT (%s at line %d)", exit_name,
                   exit_line);
    }
    else
    {
        M_snprintf(what, sizeof(what), "GO TO (%.0f, %.0f)", goto_x, goto_y);
    }

    if (priority != PRIORITY_IDLE && path != NULL && K_AutoplayEnabled())
    {
        M_snprintf(status, sizeof(status), "%s - %d waypoints left, %.0f units",
                   what, path->num_points - nav->target, nav->remaining);
    }
    else if (priority != PRIORITY_IDLE && path != NULL)
    {
        M_snprintf(status, sizeof(status), "%s%s - %d waypoints, %.0f units", mode,
                   what, path->num_points - 1, path->length);
    }
    else if (outcome == OUTCOME_FAILED)
    {
        M_snprintf(status, sizeof(status), "%sIDLE - %s failed: %s", mode,
                   outcome_priority == PRIORITY_EXIT ? "GO TO EXIT" : what, failure);
    }
    else if (outcome == OUTCOME_DONE)
    {
        M_snprintf(status, sizeof(status), "IDLE - %s done in %.1f s", what,
                   (double) outcome_tics / TICRATE);
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
