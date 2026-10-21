//
// DESCRIPTION:
//	Karp navigation. So far it only plans: the route is drawn in the
//	console, and nothing drives the player yet.
//

#include "doomtype.h"

#include "k_log.h"
#include "k_nav.h"

static k_path_t path;
static boolean have_goal;

void K_Nav_Reset(void)
{
    have_goal = false;
}

int K_Nav_SetGoal(double x, double y)
{
    char text[K_LOG_LINE_LEN];
    k_vec2_t start, goal;
    k_pose_t player;

    if (!K_Map_GetPlayer(&player))
    {
        have_goal = false;
        return 0;
    }

    start.x = player.x;
    start.y = player.y;
    goal.x = x;
    goal.y = y;

    K_Path_Find(&path, start, goal);
    have_goal = true;

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
    return 0;
}

const k_path_t *K_Nav_GetPath(void)
{
    return have_goal ? &path : NULL;
}
