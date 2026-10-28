//
// DESCRIPTION:
//	Karp player trace (k_trace.h). Read-only, like the rest of Karp: each
//	tic it compares what it sees with the tic before, and logs the
//	differences under PLAYER.
//

#include <math.h>
#include <stdlib.h>

#include "SDL.h"

#include "d_event.h"
#include "doomdef.h"
#include "doomstat.h"
#include "r_state.h"

#include "k_log.h"
#include "k_map.h"
#include "k_trace.h"

#define K_PI 3.14159265358979323846

// A point on the trail every TRAIL_STEP units, and whenever the player
// gets into another sector. A move longer than TELEPORT_JUMP in one tic is
// a teleport.
#define TRAIL_STEP 256.0
#define TELEPORT_JUMP 100.0

// Use reaches this far straight ahead (USERANGE); it stops at the first
// line with a special, or at one with no opening (PTR_UseTraverse).
#define USE_RANGE 64.0
#define MAX_USE_HITS 32

static const char *weapon_names[NUMWEAPONS] =
{
    "fist", "pistol", "shotgun", "chaingun", "rocket launcher", "plasma rifle",
    "BFG 9000", "chainsaw", "super shotgun",
};

static const char *card_names[NUMCARDS] =
{
    "blue keycard", "yellow keycard", "red keycard",
    "blue skull key", "yellow skull key", "red skull key",
};

// What the player and the level were like last tic.
static boolean started;
static double last_x, last_y;
static double trail_x, trail_y;     // the last point on the trail
static int last_sector;
static int last_buttons;
static int last_health;
static boolean last_cards[NUMCARDS];
static boolean last_weapons[NUMWEAPONS];
static int last_ready;
static short *last_specials;        // per line: one-shot lines lose theirs
static unsigned char *moving;       // per sector: a mover thinker
static unsigned char *standing;     // per solid thing (k_map blocks)

typedef struct
{
    double t;                       // along the use trace
    int line;
} hit_t;

void K_Trace_Reset(void)
{
    const k_map_t *map = K_Map_Get();

    free(last_specials);
    free(moving);
    free(standing);
    last_specials = calloc(SDL_max(numlines, 1), sizeof(*last_specials));
    moving = calloc(SDL_max(numsectors, 1), 1);
    standing = calloc(SDL_max(map->num_blocks, 1), 1);
    started = false;
}

static int SectorOf(const mobj_t *mo)
{
    return (int) (mo->subsector->sector - sectors);
}

static double Degrees(angle_t angle)
{
    return angle / 4294967296.0 * 360.0;
}

static double SectorFloor(int sector)
{
    return sectors[sector].floorheight / (double) FRACUNIT;
}

static double SectorCeiling(int sector)
{
    return sectors[sector].ceilingheight / (double) FRACUNIT;
}

// Where along segment ab it crosses segment cd (0..1), or -1.
static double Crossing(double ax, double ay, double bx, double by,
                       double cx, double cy, double dx, double dy)
{
    double rx = bx - ax, ry = by - ay, sx = dx - cx, sy = dy - cy;
    double denominator = rx * sy - ry * sx;
    double t, u;

    if (fabs(denominator) < 1e-9)
    {
        return -1.0;
    }

    t = ((cx - ax) * sy - (cy - ay) * sx) / denominator;
    u = ((cx - ax) * ry - (cy - ay) * rx) / denominator;
    return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0 ? t : -1.0;
}

static int CompareHits(const void *a, const void *b)
{
    double ta = ((const hit_t *) a)->t, tb = ((const hit_t *) b)->t;

    return ta < tb ? -1 : ta > tb;
}

// What a press of use reaches, as P_UseLines traces it. The press was
// handled earlier in the tic, so a one-shot line has lost its special
// already: last tic's specials tell.
static void LogUse(const mobj_t *mo, double x, double y)
{
    const k_map_t *map = K_Map_Get();
    double angle = mo->angle / 4294967296.0 * 2.0 * K_PI;
    double ex = x + cos(angle) * USE_RANGE, ey = y + sin(angle) * USE_RANGE;
    hit_t hits[MAX_USE_HITS];
    int count = 0;
    int i;

    for (i = 0; i < map->num_lines && count < MAX_USE_HITS; ++i)
    {
        const k_line_t *line = &map->lines[i];
        double t = Crossing(x, y, ex, ey, line->v1.x, line->v1.y, line->v2.x, line->v2.y);

        if (t > 0.0)
        {
            hits[count].t = t;
            hits[count].line = i;
            ++count;
        }
    }

    qsort(hits, count, sizeof(*hits), CompareHits);

    for (i = 0; i < count; ++i)
    {
        const line_t *line = &lines[hits[i].line];
        fixed_t top, bottom;

        if (last_specials[hits[i].line] != 0)
        {
            const k_line_t *l = &map->lines[hits[i].line];
            boolean front = (l->v2.x - l->v1.x) * (y - l->v1.y)
                          - (l->v2.y - l->v1.y) * (x - l->v1.x) <= 0.0;

            K_Log(K_LOG_PLAYER, K_LOG_INFO, "uses line %d (special %d, tag %d) from its %s, "
                  "at (%.0f,%.0f) facing %.0f", hits[i].line, last_specials[hits[i].line],
                  line->tag, front ? "front" : "back", x, y, Degrees(mo->angle));
            return;
        }

        if (line->backsector == NULL)
        {
            break;
        }

        top = SDL_min(line->frontsector->ceilingheight, line->backsector->ceilingheight);
        bottom = SDL_max(line->frontsector->floorheight, line->backsector->floorheight);

        if (top <= bottom)
        {
            break;
        }
    }

    if (i < count)
    {
        K_Log(K_LOG_PLAYER, K_LOG_INFO, "uses nothing - line %d is in the way, at (%.0f,%.0f) "
              "facing %.0f", hits[i].line, x, y, Degrees(mo->angle));
    }
    else
    {
        K_Log(K_LOG_PLAYER, K_LOG_INFO, "uses nothing in reach, at (%.0f,%.0f) facing %.0f",
              x, y, Degrees(mo->angle));
    }
}

static void Start(const player_t *player, double x, double y)
{
    const k_map_t *map = K_Map_Get();
    int i;

    last_x = trail_x = x;
    last_y = trail_y = y;
    last_sector = SectorOf(player->mo);
    last_buttons = player->cmd.buttons;
    last_health = player->health;
    last_ready = player->readyweapon;

    for (i = 0; i < NUMCARDS; ++i)
    {
        last_cards[i] = player->cards[i];
    }

    for (i = 0; i < NUMWEAPONS; ++i)
    {
        last_weapons[i] = player->weaponowned[i];
    }

    for (i = 0; i < numlines; ++i)
    {
        last_specials[i] = lines[i].special;
    }

    for (i = 0; i < numsectors; ++i)
    {
        moving[i] = sectors[i].specialdata != NULL;
    }

    for (i = 0; i < map->num_blocks; ++i)
    {
        standing[i] = K_Map_BlockStands(i) != 0;
    }

    started = true;
    K_Log(K_LOG_PLAYER, K_LOG_INFO, "starts at (%.0f,%.0f) in sector %d facing %.0f, "
          "health %d, %s in hand", x, y, last_sector, Degrees(player->mo->angle),
          player->health, weapon_names[player->readyweapon]);
}

// Where the player goes: teleports, special lines crossed, sectors entered,
// and a point every TRAIL_STEP units.
static void TraceMove(double x, double y, int sector)
{
    const k_map_t *map = K_Map_Get();
    int i;

    if (hypot(x - last_x, y - last_y) > TELEPORT_JUMP)
    {
        K_Log(K_LOG_PLAYER, K_LOG_INFO, "teleported from (%.0f,%.0f) to (%.0f,%.0f), "
              "sector %d", last_x, last_y, x, y, sector);
        trail_x = x;
        trail_y = y;
        return;
    }

    // Last tic's specials: a walk-over line used once loses its special in
    // the tic it is crossed.
    for (i = 0; i < map->num_lines; ++i)
    {
        const k_line_t *line = &map->lines[i];

        if (last_specials[i] != 0
         && Crossing(last_x, last_y, x, y, line->v1.x, line->v1.y,
                     line->v2.x, line->v2.y) >= 0.0)
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "crosses line %d (special %d, tag %d) at "
                  "(%.0f,%.0f)", i, last_specials[i], lines[i].tag, x, y);
        }
    }

    if (sector != last_sector)
    {
        K_Log(K_LOG_PLAYER, K_LOG_INFO, "in sector %d (floor %.0f, ceiling %.0f) at "
              "(%.0f,%.0f)", sector, SectorFloor(sector), SectorCeiling(sector), x, y);
        trail_x = x;
        trail_y = y;
    }
    else if (hypot(x - trail_x, y - trail_y) >= TRAIL_STEP)
    {
        K_Log(K_LOG_PLAYER, K_LOG_INFO, "at (%.0f,%.0f)", x, y);
        trail_x = x;
        trail_y = y;
    }
}

// What changed in the level: lines used up, sectors that start or stop
// moving, solid things gone (barrels blown up).
static void TraceLevel(void)
{
    const k_map_t *map = K_Map_Get();
    int i;

    for (i = 0; i < numlines; ++i)
    {
        if (last_specials[i] != 0 && lines[i].special == 0)
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "line %d used up (special %d, tag %d)", i,
                  last_specials[i], lines[i].tag);
        }

        last_specials[i] = lines[i].special;
    }

    for (i = 0; i < numsectors; ++i)
    {
        boolean now = sectors[i].specialdata != NULL;

        if (now != moving[i])
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "sector %d (tag %d) %s, floor %.0f, ceiling %.0f",
                  i, sectors[i].tag, now ? "starts moving" : "stopped", SectorFloor(i),
                  SectorCeiling(i));
            moving[i] = now;
        }
    }

    for (i = 0; i < map->num_blocks; ++i)
    {
        boolean now = K_Map_BlockStands(i) != 0;

        if (standing[i] && !now)
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "%s at (%.0f,%.0f) gone",
                  map->blocks[i].barrel ? "barrel" : "solid thing",
                  map->blocks[i].at.x, map->blocks[i].at.y);
        }

        standing[i] = now;
    }
}

// What happened to the player: use and fire pressed, damage and health,
// keys and weapons.
static void TracePlayer(const player_t *player, double x, double y)
{
    int buttons = player->cmd.buttons;
    int i;

    // A special command (pause, save) uses the other bits for its own ends.
    if (!(buttons & BT_SPECIAL))
    {
        if ((buttons & BT_USE) && !(last_buttons & BT_USE))
        {
            LogUse(player->mo, x, y);
        }

        if ((buttons & BT_ATTACK) && !(last_buttons & BT_ATTACK))
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "fires the %s at (%.0f,%.0f) facing %.0f",
                  weapon_names[player->readyweapon], x, y, Degrees(player->mo->angle));
        }

        last_buttons = buttons;
    }

    if (player->health != last_health)
    {
        K_Log(K_LOG_PLAYER, player->health < last_health ? K_LOG_WARN : K_LOG_INFO,
              "health %d (%+d) at (%.0f,%.0f)", player->health,
              player->health - last_health, x, y);
        last_health = player->health;
    }

    for (i = 0; i < NUMCARDS; ++i)
    {
        if (player->cards[i] && !last_cards[i])
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "picks up the %s at (%.0f,%.0f)", card_names[i],
                  x, y);
        }

        last_cards[i] = player->cards[i];
    }

    for (i = 0; i < NUMWEAPONS; ++i)
    {
        if (player->weaponowned[i] && !last_weapons[i])
        {
            K_Log(K_LOG_PLAYER, K_LOG_INFO, "picks up the %s at (%.0f,%.0f)", weapon_names[i],
                  x, y);
        }

        last_weapons[i] = player->weaponowned[i];
    }

    if ((int) player->readyweapon != last_ready)
    {
        K_Log(K_LOG_PLAYER, K_LOG_INFO, "%s in hand", weapon_names[player->readyweapon]);
        last_ready = player->readyweapon;
    }
}

void K_Trace_Tick(void)
{
    const player_t *player = &players[consoleplayer];
    double x, y;
    int sector;

    if (player->mo == NULL || last_specials == NULL)
    {
        return;
    }

    x = player->mo->x / (double) FRACUNIT;
    y = player->mo->y / (double) FRACUNIT;
    sector = SectorOf(player->mo);

    if (!started)
    {
        Start(player, x, y);
        return;
    }

    TraceMove(x, y, sector);
    TracePlayer(player, x, y);
    TraceLevel();

    last_x = x;
    last_y = y;
    last_sector = sector;
}
