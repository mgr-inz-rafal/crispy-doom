//
// DESCRIPTION:
//	Karp priority system. So far a single GOTO at a time, set by clicking
//	the console's map.
//

#include "doomdef.h"
#include "m_misc.h"

#include "karp.h"
#include "k_brain.h"
#include "k_log.h"
#include "k_nav.h"

typedef enum
{
    PRIORITY_IDLE,
    PRIORITY_GOTO,
} priority_type_t;

// How the last GOTO ended.
typedef enum
{
    OUTCOME_NONE,
    OUTCOME_DONE,
    OUTCOME_FAILED,
} outcome_t;

static priority_type_t priority;
static double goto_x, goto_y;

static outcome_t outcome;
static int outcome_tics;
static char failure[K_LOG_LINE_LEN];

static char status[K_LOG_LINE_LEN + 64];

static void FailGoto(void)
{
    M_StringCopy(failure, K_Nav_GetStatus()->problem, sizeof(failure));
    priority = PRIORITY_IDLE;
    outcome = OUTCOME_FAILED;
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "priority GOTO failed - %s", failure);
}

void K_Brain_Reset(void)
{
    if (priority == PRIORITY_GOTO)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GOTO (%.0f,%.0f) dropped - "
              "new level", goto_x, goto_y);
    }

    priority = PRIORITY_IDLE;
    outcome = OUTCOME_NONE;
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

    priority = PRIORITY_GOTO;
    goto_x = x;
    goto_y = y;
    outcome = OUTCOME_NONE;

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "new priority GOTO (%.0f,%.0f) - user click",
          x, y);

    if (!K_Nav_SetGoal(x, y))
    {
        FailGoto();
    }
}

void K_Brain_Tick(void)
{
    if (priority != PRIORITY_GOTO)
    {
        return;
    }

    switch (K_Nav_Tick())
    {
        case K_NAV_ARRIVED:
            priority = PRIORITY_IDLE;
            outcome = OUTCOME_DONE;
            outcome_tics = K_Nav_GetStatus()->tics;
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GOTO (%.0f,%.0f) done - "
                  "arrived in %.1f s", goto_x, goto_y,
                  (double) outcome_tics / TICRATE);
            break;

        case K_NAV_FAILED:
            FailGoto();
            break;

        default:
            break;
    }
}

const char *K_Brain_Status(void)
{
    const char *mode = K_AutoplayEnabled() ? "" : "VIEWER - ";
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();

    if (priority == PRIORITY_GOTO && path != NULL && K_AutoplayEnabled())
    {
        M_snprintf(status, sizeof(status), "GO TO (%.0f, %.0f) - %d waypoints "
                   "left, %.0f units", goto_x, goto_y,
                   path->num_points - nav->target, nav->remaining);
    }
    else if (priority == PRIORITY_GOTO && path != NULL)
    {
        M_snprintf(status, sizeof(status), "%sGO TO (%.0f, %.0f) - %d waypoints, "
                   "%.0f units", mode, goto_x, goto_y, path->num_points - 1,
                   path->length);
    }
    else if (outcome == OUTCOME_FAILED)
    {
        M_snprintf(status, sizeof(status), "%sIDLE - GO TO (%.0f, %.0f) failed: "
                   "%s", mode, goto_x, goto_y, failure);
    }
    else if (outcome == OUTCOME_DONE)
    {
        M_snprintf(status, sizeof(status), "IDLE - GO TO (%.0f, %.0f) done in "
                   "%.1f s", goto_x, goto_y, (double) outcome_tics / TICRATE);
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
