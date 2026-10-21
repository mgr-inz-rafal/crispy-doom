//
// DESCRIPTION:
//	Karp priority system. So far a single GOTO at a time, set by clicking
//	the console's map; it is planned but not yet driven.
//

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

static priority_type_t priority;
static double goto_x, goto_y;

// The last GOTO failed (the bot is IDLE again), and why.
static boolean failed;
static char failure[K_LOG_LINE_LEN];

static char status[K_LOG_LINE_LEN + 64];

void K_Brain_Reset(void)
{
    if (priority == PRIORITY_GOTO)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "priority GOTO (%.0f,%.0f) dropped - "
              "new level", goto_x, goto_y);
    }

    priority = PRIORITY_IDLE;
    failed = false;
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
    failed = false;

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "new priority GOTO (%.0f,%.0f) - user click",
          x, y);

    if (!K_Nav_SetGoal(x, y))
    {
        const k_path_t *path = K_Nav_GetPath();

        if (path != NULL)
        {
            K_Path_Describe(path, failure, sizeof(failure));
        }
        else
        {
            M_StringCopy(failure, "not in a level", sizeof(failure));
        }

        priority = PRIORITY_IDLE;
        failed = true;
        K_Log(K_LOG_BRAIN, K_LOG_WARN, "priority GOTO failed - no path: %s",
              failure);
    }
}

const char *K_Brain_Status(void)
{
    const char *mode = K_AutoplayEnabled() ? "" : "VIEWER - ";
    const k_path_t *path = K_Nav_GetPath();

    if (priority == PRIORITY_GOTO && path != NULL)
    {
        M_snprintf(status, sizeof(status), "%sGO TO (%.0f, %.0f) - %d waypoints, "
                   "%.0f units", mode, goto_x, goto_y, path->num_points - 1,
                   path->length);
    }
    else if (failed)
    {
        M_snprintf(status, sizeof(status), "%sIDLE - GO TO (%.0f, %.0f) failed: "
                   "%s", mode, goto_x, goto_y, failure);
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
