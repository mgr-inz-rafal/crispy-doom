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
//	moves, KILL EVERY <kind> for one a boss's death moves (E1M8's barons).
//	When that is done it comes off, and the goal below is planned
//	again from wherever the player is: the world has changed.
//
//	A goal is done when its condition holds, however that came about. A
//	candidate that fails is not chosen again for a while; a goal whose
//	candidates all fail fails, is not pushed again for a while, and the
//	goal below looks for another way. Nothing is given up for good: GO TO
//	EXIT never fails, and with no way it waits and plans again. A click on
//	the console's map pushes a GOTO on top; when it ends, the rest resumes.
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
#include "k_area.h"
#include "k_brain.h"
#include "k_combat.h"
#include "k_condition.h"
#include "k_items.h"
#include "k_jump.h"
#include "k_log.h"
#include "k_nav.h"
#include "k_sight.h"
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

// A goal whose way shut while the player was on it (a door closing before
// it got through) is planned again, which pushes the fix, up to this many
// times before the candidate counts as not working out.
#define MAX_SHUT 3

// No way to any candidate while sectors are still moving (stairs being
// built, a floor going down): plan again after RETRY_TICS, for up to
// MAX_WAIT_TICS in all before the goal fails.
#define RETRY_TICS TICRATE
#define MAX_WAIT_TICS (15 * TICRATE)

// Nothing is given up for good (follow-up 11, the user): something out of
// the player's hands may still open a way - a monster crossing a walk-over
// line, a slow floor, a door bounced off a monster - or a second try go
// better than the first. A goal that failed may go on the stack again after
// FAIL_TICS, twice that after failing again, and so on up to MAX_FAIL_TICS;
// a candidate that did not work out is tried again after TRIED_TICS; a
// line's uses (MAX_TRIGGERS) count for LINE_USES_TICS after the last one.
// GO TO EXIT never fails: with no way, it is planned again every IDLE_TICS
// (Doom II MAP02: stuck by a barrel for 8 s, the blue key was given up for
// good, then the exit, and the bot stood idle).
#define FAIL_TICS (30 * TICRATE)
#define MAX_FAIL_TICS (240 * TICRATE)
#define TRIED_TICS (30 * TICRATE)
#define LINE_USES_TICS (30 * TICRATE)
#define IDLE_TICS (5 * TICRATE)

// A door that leaves less headroom than the player's height is shut.
#define PLAYER_HEIGHT 56.0

// A barrel in the way (plan, 7.3) goes off with its chain: the barrels under
// BARREL_CHAIN apart that see each other, up to MAX_CHAIN. Any of them may be
// the one shot (the user: MAP23's are placed so that shooting the one in
// the way hurts; the user shot one on a ledge from outside the yellow door,
// and the rest went off out of sight). It is shot from a spot BARREL_RINGS
// from it, in BARREL_WAYS directions, that the player can stand at, in reach
// of the shots, and safe from every barrel of the chain: BARREL_SAFE off, or
// out of its sight (a blast hurts only what can see it; a dead barrel is a
// quarter of its height, BLAST_HEIGHT). Shooting gives up after
// BARREL_SHOOT_TICS out of a fight (combat shoots until it goes off, or
// misses too often). Once it is gone, the player goes on only when no shot
// barrel within BARREL_CHAIN_WAIT of it or of the player is still to go off.
#define BARREL_WAYS 16
#define BARREL_SAFE 160.0
#define BARREL_CHAIN 118.0
#define MAX_CHAIN 32
#define BARREL_SHOOT_TICS (10 * TICRATE)
#define BARREL_CHAIN_WAIT 400.0
#define GUN_HEIGHT 36.0
#define EYE_HEIGHT 42.0
#define BARREL_HEIGHT 42.0
#define BLAST_HEIGHT 10.0

static const double barrel_rings[] = { 192.0, 256.0, 320.0 };

// Items worth a detour (k_items.h, plan 7.5) are looked for every
// ITEM_CHECK_TICS while the player follows a route, the ITEM_ROUTES most
// promising by worth and distance looked at by route (MAX_ITEM_ROUTES when
// health is CRITICAL, any detour then being worth it); one with no open way
// there, or none on from it, is not looked at again for ITEM_SKIP_TICS. The
// distance is as the crow flies, or twice that from the route within
// ROUTE_AHEAD ahead if less: an item by the route ahead costs a step aside,
// however far off it is (Doom II MAP31: the clips of two SS just killed lay
// by the route 340 off, past the 256 they were worth, and were run past).
#define ITEM_CHECK_TICS TICRATE
#define ITEM_ROUTES 2
#define MAX_ITEM_ROUTES 4
#define ITEM_SKIP_TICS (10 * TICRATE)
#define ROUTE_AHEAD 1024.0

// Some items go first (GoesFirst): a weapon at a pistol start (k_items.h:
// first; the user: a health bonus far off was gone for, a shotgun by the
// route left, with only the pistol in hand), and health when CRITICAL. They
// are looked at by route before any other item, the nearest first
// (FIRST_SCORE puts them ahead of any score by worth and distance), and go
// before the others when in reach. Of two such, the one in hand soonest -
// the shorter way there, the work at a fix counted - not the smaller detour
// nor the most to spare (the user: time to acquire; Doom II MAP10: a
// chaingun on the way to the key, behind a switch and a lift, was gone for
// over a shotgun back by the start, a 465-unit run, and the bot fought with
// the pistol for 20 s; at 23 health, a berserk 5322 units off over a
// medikit near, and the bot died on the way). One already gone for gives way
// to another only when the other's way there is REPLACE_MARGIN shorter, or
// the two would flip.
#define FIRST_SCORE 1.0e6
#define REPLACE_MARGIN 256.0

// In a fight only items worth breaking it off for are looked for (k_items.h:
// urgent), and but for those worth leaving it for however far (escape:
// health when LOW or CRITICAL, ammo when LOW), only within FIGHT_REACH of
// detour: a dash, shooting on the way, not a trip. Going for one, the fight
// is broken off once the item is within FIGHT_REACH (UpdateEscape) - but
// only while no monster the player can see is within DASH_CLEAR: through
// monsters close by, the fight comes first (the user, Doom II MAP02: dashing
// for a super shotgun, the bot walked into a room of zombiemen and shotgun
// guys 40 to 90 off, fighting none of them, and died).
#define FIGHT_REACH 400.0
#define DASH_CLEAR 128.0

// Out of a fight, an item behind one fix the brain knows - barrels in the
// way, a door or bars a switch or walk-over line opens; never a key - is
// gone for through it: the detour counts the way to the fix's nearest spot,
// on to the item and on, and FIX_WORK for the work there (the user, Doom II
// MAP04: the shotguns dropped behind barrels in a doorway, and a super
// shotgun behind a door a switch opens, were left with the shells gone).
#define FIX_WORK 300.0

// The floors that hurt on a detour (k_path.h: damage) - there and on, less
// what the route takes anyway - count against it, each point of health as
// DAMAGE_WAY units of detour, as the pathfinder counts them; and health is
// gone for only when they take no more than HEALTH_NET of what it gives
// (the user: on levels with large pools of nukage, stimpacks chased across
// it cost more health than they gave).
#define DAMAGE_WAY 100.0
#define HEALTH_NET 0.5

// So do the monsters inside the areas a detour goes through that the route
// left does not (k_area.h: inside, the asleep at half) - not the item's own
// area, whose guards its worth counts already (k_items.h: danger) - each
// point of health as DAMAGE_WAY units of detour (the user: detours took the
// bot into fresh fights on many fronts, waking monsters). An item any detour
// pays for (k_items.h: reach - health LOW, the ammo running out) pays for
// no more than RISKY_REACH while they come to RISKY_ANY or more (MAP10: a
// stimpack at LOW health, 18062 units out of the way through 125 health of
// monsters); but for health CRITICAL, when staying costs as much.
#define ANY_REACH 1.0e5
#define RISKY_ANY 20.0
#define RISKY_REACH 3000.0

// Ways count the waits at doors, lifts and teleporters on them, as the
// pathfinder expects them (k_path.h: wait), each tic as WAIT_WAY units, the
// way the player could run meanwhile: an item behind a slow lift is further
// off than its length.
#define WAIT_WAY 16.0

// An item whose way there waits more than ITEM_WAIT_MOST at doors and
// lifts is not gone for, for now (E1M6: at LOW health, any detour paying,
// the bot stood 25.6 s at a door shut on a timer for a stimpack behind it,
// and went for a medikit 1200 units off right after).
#define ITEM_WAIT_MOST (8 * TICRATE)

// A radiation suit (k_items.h: suit) is also worth the detour when going on
// by it - the way there, then on with the suit worn - costs SUIT_GAIN less
// than going on without, the floors counted as above: those it would keep
// out on the route ahead, or a way round a pool cut short through it (the
// user: worth a detour for long stretches of nukage, and lying in the
// nukage more often than not). About 5 health: the reckoning is rough, and
// a little saved is no trip (Doom II MAP29: 353 saved, and a lift that took
// 24 s); MAP24's suit by the nukage run below the ledge saves 768, and the
// bot keeps 100 health through it (70 without). Not while one is worn with
// SUIT_KEEP tics left.
#define SUIT_GAIN 500.0
#define SUIT_KEEP (20 * TICRATE)

// Held up by a barrel where the map shows room to pass (the user, E1M5:
// barrels in a doorway held the bot for seconds before it squeezed past):
// following a route, with nothing to work and no fight, the player kept
// within HOLD_RADIUS of one spot for HOLD_TICS, a standing barrel within
// HOLD_GAP of its box - that barrel is shot from a safe spot, as one in the
// way (GOAL_BARREL).
#define HOLD_TICS (5 * TICRATE / 2)
#define HOLD_RADIUS 48.0
#define HOLD_GAP 24.0
#define PLAYER_RADIUS 16.0

typedef enum
{
    GOAL_GOTO,              // a point (a click)
    GOAL_EXIT,              // any exit line
    GOAL_KEY,               // any key of a colour
    GOAL_TRIGGER,           // any line that moves a sector
    GOAL_ITEM,              // an item worth a detour, on top of the chain
    GOAL_BARREL,            // a barrel in the way, shot from a safe spot
    GOAL_KILL,              // every monster of a kind dead: a map's bosses
} goal_type_t;

// A place a goal can be met, and what to do there.
typedef struct
{
    k_vec2_t at;
    k_goalkind_t kind;
    int line;               // the line to work, -1 for a key or a point
    k_vec2_t aim;           // GOAL_BARREL: the barrel to shoot from there
    char what[64];          // "exit switch at line 162", "blue keycard"...
} candidate_t;

typedef struct
{
    goal_type_t type;
    int param;              // GOAL_KEY: the key; GOAL_TRIGGER: the sector;
                            // GOAL_ITEM: the item (K_Items_Get); GOAL_BARREL:
                            // the barrel's block (k_block_t); GOAL_KILL: the
                            // kind (mobjtype_t)
    k_vec2_t point;         // GOAL_GOTO

    // Not planned until a candidate is chosen; planned again when it
    // fails, or a goal in front of this one ends.
    boolean planned;
    candidate_t chosen;
    char note[K_LOG_LINE_LEN];  // what it is doing or waiting for

    k_vec2_t tried[MAX_TRIED];  // candidates that failed
    int tried_until[MAX_TRIED]; // and when each may be tried again
    int num_tried;
    int shut;                   // times its way shut on the way (MAX_SHUT)
    boolean idle;               // GO TO EXIT: no way for now, waiting
    boolean staying;            // GO TO EXIT: on the exit floor, waiting for
                                // it to end the level
    boolean shooting;           // GOAL_BARREL: at the spot, combat shooting
                                // (wait counts the tics)
    boolean fixable;            // GOAL_ITEM: gone for through a fix in its
                                // way (FIX_WORK), pushed in front of it

    // GOAL_TRIGGER: the sector's heights when its line was gone for; or,
    // if it was busy then (settling: K_Graph_SectorBusy, a door closing by
    // itself), when it stopped.
    double floor, ceiling;
    boolean settling;
    boolean used;           // a walk-over line was crossed
    boolean moving;
    boolean changed;        // the heights have differed from those since
    int wait;

    // GOAL_TRIGGER: the blocked crossing it was pushed for (crossed out of
    // cell from), -1 if none. Once that opens, even if only after a wait
    // (a door opening, a lift on its way down), the goal is done: the
    // player goes on and waits there, not at the switch. want: the moves
    // of the sector that would open it (k_move_t); only lines that make one
    // of them are candidates. For a gap inside a cell, portal is -1, and
    // pinch_cell and pinch_at say where (else pinch_cell is -1): done once
    // the gap is wide enough.
    int portal, from;
    int want;
    int pinch_cell;
    k_vec2_t pinch_at;

    // Found no way while sectors were moving: not planned again before
    // leveltime retry_tic; waited is how long it has waited so far.
    int retry_tic;
    int waited;
} goal_t;

typedef struct
{
    goal_type_t type;
    int param;
    int count;              // times it failed this level
    int until;              // when it may go on the stack again
    char text[K_LOG_LINE_LEN];
} failed_t;

static goal_t goals[MAX_GOALS];
static int depth;           // goals[depth - 1] is worked on
static failed_t failed[MAX_FAILED];
static int num_failed;
static boolean exit_wanted; // GO TO EXIT goes on an empty list

// Refreshed when planning: the keys lying in the level (up to
// MAX_CANDIDATES of each colour known by place), and where the player was.
static int keys_lying[K_NUM_KEYS];
static k_vec2_t keys_at[K_NUM_KEYS][MAX_CANDIDATES];
static k_vec2_t planned_from;
static unsigned char *line_uses;    // per line, as a trigger goal's choice
static int *line_last;              // per line, when it was last chosen
static k_path_t scratch;
static k_path_t probe;              // routes looked at while scratch is in use
static k_path_t fix_probe;          // routes to and from a fix an item is behind

// Areas (k_area.h) the route left goes through, marked route_stamp; those a
// detour has counted already, marked risk_stamp.
static int *route_marks;
static int *risk_marks;
static int marks_size;
static int route_stamp;
static int risk_stamp;
static candidate_t fix_candidates[MAX_CANDIDATES];

static char outcome[K_LOG_LINE_LEN];    // how the list last emptied
static char status[K_LOG_LINE_LEN * 2];
static int item_check_tic;              // when to look for items next
static boolean was_critical;            // health was CRITICAL at the last look
static int dashing = -1;                // the item fights are broken off for
static int *item_skip;                  // per item: not looked at before this
static int item_skip_size;
static k_vec2_t hold_at;                // where the player has kept near, and
static int hold_since;                  // since when (HOLD_TICS)

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

        case GOAL_ITEM:
            M_snprintf(buf, len, "GET %s", K_Items_Get(goal->param)->name);
            M_ForceUppercase(buf);
            break;

        case GOAL_BARREL:
            M_snprintf(buf, len, "SHOOT BARREL at (%.0f,%.0f)",
                       K_Map_Get()->blocks[goal->param].at.x,
                       K_Map_Get()->blocks[goal->param].at.y);
            break;

        case GOAL_KILL:
            M_snprintf(buf, len, "KILL EVERY %s", K_Combat_KindName(goal->param));
            M_ForceUppercase(buf);
            break;

        // The line's tag: a stair builder's steps after the first are not
        // tagged.
        case GOAL_TRIGGER:
            // By the special at level load: an open-and-stay door loses its
            // own once used.
            if (goal->planned && goal->chosen.kind == K_GOAL_USE_LINE
             && K_Specials_ManualDoor(K_Map_Get()->lines[goal->chosen.line].special))
            {
                M_snprintf(buf, len, "OPEN DOOR line %d (sector %d)", goal->chosen.line,
                           goal->param);
            }
            else if (goal->planned && goal->chosen.kind == K_GOAL_USE_LINE)
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

static failed_t *FailedEntry(goal_type_t type, int param)
{
    int i;

    for (i = 0; i < num_failed; ++i)
    {
        if (failed[i].type == type && failed[i].param == param)
        {
            return &failed[i];
        }
    }

    return NULL;
}

// Failed, and not yet due to be tried again.
static boolean IsFailed(goal_type_t type, int param)
{
    const failed_t *entry = FailedEntry(type, param);

    return entry != NULL && leveltime < entry->until;
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

// How many times a line was chosen lately: the count starts over
// LINE_USES_TICS after the last time.
static int LineUses(int line)
{
    if (line_uses == NULL)
    {
        return 0;
    }

    if (leveltime - line_last[line] >= LINE_USES_TICS)
    {
        line_uses[line] = 0;
    }

    return line_uses[line];
}

// Whether a line can still be worked: it has its special (one-shot lines
// lose it once used) and has not been worked MAX_TRIGGERS times lately.
static boolean LineWorks(int line)
{
    return K_Specials_MoverTrigger(K_Specials_LineSpecial(line)) != K_TRIGGER_NONE
        && LineUses(line) < MAX_TRIGGERS;
}

// Whether a line still works and moves its sectors one of the ways wanted
// (k_move_t flags).
static boolean LineHelps(int line, int want)
{
    return LineWorks(line) && (K_Specials_MoverMoves(K_Specials_LineSpecial(line)) & want);
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
    goal->pinch_cell = -1;
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

// Not pushed again for a while: FAIL_TICS, doubled each time it fails
// again, up to MAX_FAIL_TICS. Never GO TO EXIT (PlanGoal waits instead).
static void FailGoal(const char *why)
{
    goal_t *goal = &goals[depth - 1];
    failed_t *entry = FailedEntry(goal->type, goal->param);
    char name[64];
    int tics, i;

    // A new entry; when the list is full, in place of the one due first.
    if (entry == NULL && num_failed < MAX_FAILED)
    {
        entry = &failed[num_failed++];
        entry->count = 0;
    }
    else if (entry == NULL)
    {
        for (entry = &failed[0], i = 1; i < num_failed; ++i)
        {
            if (failed[i].until < entry->until)
            {
                entry = &failed[i];
            }
        }

        entry->count = 0;
    }

    entry->type = goal->type;
    entry->param = goal->param;
    tics = FAIL_TICS << SDL_min(entry->count, 3);
    tics = SDL_min(tics, MAX_FAIL_TICS);
    entry->until = leveltime + tics;
    ++entry->count;

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s failed - %s; trying again in %d s", name, why,
          tics / TICRATE);
    M_snprintf(entry->text, sizeof(entry->text), "%s - %s", name, why);

    if (depth == 1)
    {
        M_snprintf(outcome, sizeof(outcome), "%s failed - %s", name, why);
    }

    Pop();
}

// A candidate tried at the point, -1 if none.
static int TriedIndex(const goal_t *goal, k_vec2_t at)
{
    int i;

    for (i = 0; i < goal->num_tried; ++i)
    {
        if (fabs(goal->tried[i].x - at.x) < 1.0 && fabs(goal->tried[i].y - at.y) < 1.0)
        {
            return i;
        }
    }

    return -1;
}

// The chosen candidate did not work out: plan again without it, for
// TRIED_TICS.
static void CandidateFailed(goal_t *goal, const char *why)
{
    char name[64];
    int i = TriedIndex(goal, goal->chosen.at);

    GoalName(goal, name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: the %s did not work out - %s", name,
          goal->chosen.what, why);

    // Its own entry again, else a new one, else the one due first.
    if (i < 0 && goal->num_tried < MAX_TRIED)
    {
        i = goal->num_tried++;
    }
    else if (i < 0)
    {
        int k;

        for (i = 0, k = 1; k < goal->num_tried; ++k)
        {
            if (goal->tried_until[k] < goal->tried_until[i])
            {
                i = k;
            }
        }
    }

    goal->tried[i] = goal->chosen.at;
    goal->tried_until[i] = leveltime + TRIED_TICS;
    goal->planned = false;
    K_Nav_Clear();
}

//
// Candidates
//

// Tried lately and did not work out.
static boolean WasTried(const goal_t *goal, k_vec2_t at)
{
    int i = TriedIndex(goal, at);

    return i >= 0 && leveltime < goal->tried_until[i];
}

// The floor a block stands on.
static double BlockFloor(int block)
{
    const k_block_t *b = &K_Map_Get()->blocks[block];
    int cell = K_Map_CellAt(b->at.x, b->at.y);

    return cell >= 0 ? K_Map_CellFloor(cell) : 0.0;
}

static double AxisDistance(k_vec2_t a, k_vec2_t b)
{
    return SDL_max(fabs(b.x - a.x), fabs(b.y - a.y));
}

// The standing barrels that go off with the barrel (BARREL_CHAIN), itself
// first. Returns how many.
static int BarrelChain(int block, int *chain)
{
    const k_map_t *map = K_Map_Get();
    int count = 1;
    int i, b, k;

    chain[0] = block;

    for (i = 0; i < count; ++i)
    {
        const k_block_t *bomb = &map->blocks[chain[i]];
        double bomb_floor = BlockFloor(chain[i]);

        for (b = 0; b < map->num_blocks && count < MAX_CHAIN; ++b)
        {
            const k_block_t *other = &map->blocks[b];

            for (k = 0; k < count && chain[k] != b; ++k)
            {
            }

            // P_RadiusAttack: the barrel hit must see the one going off.
            if (k < count || !other->barrel || !K_Map_BlockStands(b)
             || AxisDistance(bomb->at, other->at) >= BARREL_CHAIN
             || !K_Sight_Check(other->at.x, other->at.y,
                               BlockFloor(b) + BARREL_HEIGHT * 0.75, bomb->at.x, bomb->at.y,
                               bomb_floor, bomb_floor + BLAST_HEIGHT))
            {
                continue;
            }

            chain[count++] = b;
        }
    }

    return count;
}

// Spots to set off a barrel in the way from (BARREL_RINGS): round any
// barrel of its chain, on floor the player can stand on, that barrel in
// reach of the shots, every barrel of the chain BARREL_SAFE off or out of
// sight. Returns how many.
static int BarrelSpots(int block, candidate_t *out, int max)
{
    const k_map_t *map = K_Map_Get();
    int chain[MAX_CHAIN];
    int length = BarrelChain(block, chain);
    int count = 0;
    int m, r, k, c;

    for (m = 0; m < length; ++m)
    {
        const k_block_t *barrel = &map->blocks[chain[m]];
        double bottom = BlockFloor(chain[m]);

        for (r = 0; r < arrlen(barrel_rings); ++r)
        {
            for (k = 0; k < BARREL_WAYS && count < max; ++k)
            {
                double angle = k * 2.0 * 3.14159265358979323846 / BARREL_WAYS;
                k_vec2_t spot = { barrel->at.x + cos(angle) * barrel_rings[r],
                                  barrel->at.y + sin(angle) * barrel_rings[r] };
                int cell = K_Map_CellAt(spot.x, spot.y);
                boolean safe = true;
                double floor;

                if (cell < 0 || K_Graph_Clearance(spot, 17.0) < 17.0)
                {
                    continue;
                }

                floor = K_Map_CellFloor(cell);

                if (!K_Sight_Aim(spot.x, spot.y, floor + GUN_HEIGHT, barrel->at.x, barrel->at.y,
                                 bottom, bottom + BARREL_HEIGHT))
                {
                    continue;
                }

                for (c = 0; c < length && safe; ++c)
                {
                    const k_block_t *other = &map->blocks[chain[c]];
                    double other_floor = BlockFloor(chain[c]);

                    safe = AxisDistance(spot, other->at) >= BARREL_SAFE
                        || !K_Sight_Check(spot.x, spot.y, floor + EYE_HEIGHT, other->at.x,
                                          other->at.y, other_floor, other_floor + BLAST_HEIGHT);
                }

                if (!safe)
                {
                    continue;
                }

                out[count].at = spot;
                out[count].kind = K_GOAL_POINT;
                out[count].line = -1;
                out[count].aim = barrel->at;

                if (m == 0)
                {
                    M_snprintf(out[count].what, sizeof(out[count].what),
                               "spot %.0f units from it", barrel_rings[r]);
                }
                else
                {
                    M_snprintf(out[count].what, sizeof(out[count].what),
                               "spot %.0f units from the barrel at (%.0f,%.0f) of its chain",
                               barrel_rings[r], barrel->at.x, barrel->at.y);
                }

                ++count;
            }
        }
    }

    return count;
}

// Where to go to pick up a key lying at `at` (K_Graph_PickupSpot); with no
// spot to stand at, the key itself: the route finds the way or not. A key
// on a ledge is also a candidate from the floors around it that reach it,
// MAX_KEY_FLOORS at the most (K_Graph_PickupSpotsOff): the route picks.
#define MAX_KEY_FLOORS 4

static k_vec2_t KeySpot(k_vec2_t at)
{
    k_vec2_t spot;

    return K_Graph_PickupSpot(at, &spot) ? spot : at;
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

// Which side of the line through a and b the point p is on.
static double Side(k_vec2_t a, k_vec2_t b, k_vec2_t p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Whether segments ab and cd cross.
static boolean Crosses(k_vec2_t a, k_vec2_t b, k_vec2_t c, k_vec2_t d)
{
    return (Side(c, d, a) > 0.0) != (Side(c, d, b) > 0.0)
        && (Side(a, b, c) > 0.0) != (Side(a, b, d) > 0.0);
}

// The key that a door between a switch and the spot it is pressed from
// needs, if the player lacks it, else -1; door gets the door's line. Use
// stops at the first line with a special on its way (PTR_UseTraverse), so
// a locked door in front of a switch takes the press (Doom II MAP27: switch
// 727 at the back of a closet behind the blue door). A one-shot door loses
// its special once opened, and then the press gets through.
static int DoorKeyInTheWay(const candidate_t *candidate, int *door)
{
    const k_map_t *map = K_Map_Get();
    const k_line_t *target = &map->lines[candidate->line];
    k_vec2_t middle = { (target->v1.x + target->v2.x) * 0.5,
                        (target->v1.y + target->v2.y) * 0.5 };
    int i;

    for (i = 0; i < map->num_lines; ++i)
    {
        int key;

        if (i == candidate->line
         || !Crosses(candidate->at, middle, map->lines[i].v1, map->lines[i].v2))
        {
            continue;
        }

        key = K_Specials_LockKey(K_Specials_LineSpecial(i));

        if (key >= 0 && !K_Specials_HasKey(key))
        {
            *door = i;
            return key;
        }
    }

    return -1;
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

            // A floor that ends the level (E1M8): any of its cells, the
            // route picks one it reaches (E1M8's is a box teleported into,
            // the floor round it void).
            for (i = 0; i < map->num_cells && count < max; ++i)
            {
                const k_cell_t *cell = &map->cells[i];
                int sector = map->subsectors[cell->subsector].sector;

                if (K_Specials_ExitSector(sector)
                 && K_Graph_Clearance(cell->centroid, PLAYER_RADIUS + 1.0) > PLAYER_RADIUS)
                {
                    out[count].at = cell->centroid;
                    out[count].kind = K_GOAL_POINT;
                    out[count].line = -1;
                    M_snprintf(out[count].what, sizeof(out[count].what),
                               "exit floor in sector %d at (%.0f,%.0f)", sector,
                               cell->centroid.x, cell->centroid.y);
                    ++count;
                }
            }
            break;

        case GOAL_KEY:
            {
                k_keything_t keys[MAX_CANDIDATES];
                int found = SDL_min(K_Specials_FindKeys(goal->param, keys, max), max);

                // From where it lies, or from a floor below (KeySpot).
                for (i = 0; i < found && count < max; ++i)
                {
                    k_vec2_t spots[1 + MAX_KEY_FLOORS];
                    int num_spots, k;

                    spots[0] = KeySpot(keys[i].at);
                    num_spots = 1 + K_Graph_PickupSpotsOff(keys[i].at, spots + 1,
                                                           MAX_KEY_FLOORS);

                    for (k = 0; k < num_spots && count < max; ++k)
                    {
                        out[count].at = spots[k];
                        out[count].kind = K_GOAL_POINT;
                        out[count].line = -1;
                        M_snprintf(out[count].what, sizeof(out[count].what), "%s %s at "
                                   "(%.0f,%.0f)", K_Specials_KeyName(goal->param),
                                   keys[i].skull ? "skull key" : "keycard",
                                   keys[i].at.x, keys[i].at.y);
                        ++count;
                    }
                }
            }
            break;

        case GOAL_ITEM:
            {
                const k_item_t *item = K_Items_Get(goal->param);

                if (item->present && item->reachable)
                {
                    out[0].at = item->spot;
                    out[0].kind = K_GOAL_POINT;
                    out[0].line = -1;
                    M_snprintf(out[0].what, sizeof(out[0].what), "%s at (%.0f,%.0f)",
                               item->name, item->at.x, item->at.y);
                    count = 1;
                }
            }
            break;

        case GOAL_BARREL:
            count = BarrelSpots(goal->param, out, max);
            break;

        // Where each one is now: once in sight, combat fights it.
        case GOAL_KILL:
            {
                k_vec2_t at[MAX_CANDIDATES];

                count = SDL_min(K_Specials_FindMonsters(goal->param, at, max), max);

                for (i = 0; i < count; ++i)
                {
                    out[i].at = at[i];
                    out[i].kind = K_GOAL_POINT;
                    out[i].line = -1;
                    M_snprintf(out[i].what, sizeof(out[i].what), "%s at (%.0f,%.0f)",
                               K_Combat_KindName(goal->param), at[i].x, at[i].y);
                }
            }
            break;

        case GOAL_TRIGGER:
            for (i = 0; i < K_Specials_TriggerCount(goal->param) && count < max; ++i)
            {
                int line = K_Specials_TriggerLine(goal->param, i);
                k_trigger_t trigger = K_Specials_MoverTrigger(K_Specials_LineSpecial(line));

                if (LineHelps(line, goal->want)
                 && LineCandidate(line, trigger,
                                  K_Specials_ManualDoor(K_Specials_LineSpecial(line)) ? "door"
                                  : trigger == K_TRIGGER_USE ? "switch"
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

// What opens a way with too little headroom, or too narrow: a floor going
// down, a ceiling going up.
#define OPENING_MOVES (K_MOVE_FLOOR_DOWN | K_MOVE_CEILING_UP)

// Whether some line can still move the sector one of the ways wanted
// (LineHelps).
static boolean LiveTrigger(int sector, int want)
{
    int i;

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        if (LineHelps(K_Specials_TriggerLine(sector, i), want))
        {
            return true;
        }
    }

    return false;
}

// How far the player goes to be able to use a line: nowhere if it needs no
// key or the player has it, else to the nearest key and on to the line
// (as the crow flies); -1 if there is no key to get.
static double KeyTrip(int line)
{
    const k_line_t *l = &K_Map_Get()->lines[line];
    k_vec2_t middle = { (l->v1.x + l->v2.x) * 0.5, (l->v1.y + l->v2.y) * 0.5 };
    int key = K_Specials_LockKey(K_Specials_LineSpecial(line));
    double best = -1.0;
    int i;

    if (key < 0 || K_Specials_HasKey(key))
    {
        return 0.0;
    }

    if (keys_lying[key] <= 0 || !Allowed(GOAL_KEY, key))
    {
        return -1.0;
    }

    for (i = 0; i < SDL_min(keys_lying[key], MAX_CANDIDATES); ++i)
    {
        k_vec2_t at = keys_at[key][i];
        double trip = hypot(at.x - planned_from.x, at.y - planned_from.y)
                    + hypot(middle.x - at.x, middle.y - at.y);

        if (best < 0.0 || trip < best)
        {
            best = trip;
        }
    }

    return best;
}

// What opening a sector that narrows a gap takes: nothing if a line still
// moves it out of the way (bars that lower), or its door opens from a face
// with no key or one in hand; else the trip to a key and on to the door
// (KeyTrip); -1 if nothing opens it.
static double OpeningCost(int sector)
{
    double best = -1.0;
    int i;

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        int line = K_Specials_TriggerLine(sector, i);
        double cost;

        if (LineHelps(line, OPENING_MOVES))
        {
            return 0.0;
        }

        if (!LineHelps(line, K_MOVE_DOOR))
        {
            continue;
        }

        cost = KeyTrip(line);

        if (cost >= 0.0 && (best < 0.0 || cost < best))
        {
            best = cost;
        }
    }

    return best;
}

// Of the sectors narrowing a gap, the one the player can open soonest
// (OpeningCost), as an index; -1 if none can be.
static int OpenSoonest(const int *narrowing, int count)
{
    int best = -1;
    double best_cost = 0.0;
    int i;

    for (i = 0; i < count; ++i)
    {
        double cost = OpeningCost(narrowing[i]);

        if (cost >= 0.0 && Allowed(GOAL_TRIGGER, narrowing[i])
         && (best < 0 || cost < best_cost))
        {
            best = i;
            best_cost = cost;
        }
    }

    return best;
}

// What would get the player through a gap inside a cell: as for a gap too
// narrow at a crossing, opening a sector that narrows it (Doom II MAP11: a
// fence of pillars 16 apart, the gaps between them 24 long, behind the red
// door; walk-over lines open or lower every other pillar for a while).
static boolean FixForPinch(int cell, k_vec2_t at, goal_type_t *type, int *param, int *want)
{
    int narrowing[8];
    int count = K_Graph_PinchSectors(cell, at, narrowing, arrlen(narrowing));
    int best = OpenSoonest(narrowing, count);
    int barrel = K_Graph_PinchBarrel(cell, at);

    // A barrel narrowing it: shot.
    if (barrel >= 0 && Allowed(GOAL_BARREL, barrel))
    {
        *type = GOAL_BARREL;
        *param = barrel;
        *want = 0;
        return true;
    }

    if (best < 0)
    {
        return false;
    }

    *type = GOAL_TRIGGER;
    *param = narrowing[best];
    *want = OPENING_MOVES | K_MOVE_DOOR;
    return true;
}

// The kind of boss whose death moves the sector one of the ways wanted
// (k_specials.h: A_BossDeath; E1M8: the barons lower the star-shaped wall
// before the teleporter to the exit floor), with some still alive and the
// goal allowed; -1 if none. With tag 0 (sector -1): one whose death ends
// the level (E2M8, E3M8).
static int BossFor(int sector, int want)
{
    k_boss_t bosses[4];
    int count = K_Specials_Bosses(bosses, arrlen(bosses));
    int i;

    for (i = 0; i < count; ++i)
    {
        if ((sector < 0 ? bosses[i].tag == 0
                        : bosses[i].tag != 0 && bosses[i].tag == sectors[sector].tag
                          && (bosses[i].moves & want))
         && K_Specials_FindMonsters(bosses[i].type, NULL, 0) > 0
         && Allowed(GOAL_KILL, bosses[i].type))
        {
            return bosses[i].type;
        }
    }

    return -1;
}

// What would get the player past a blocked crossing, if anything: the key
// for a locked door; for a step too high, a line that lowers the floor
// beyond or raises the one the player is on, never one that lowers it (Doom
// II MAP27: lowering the arm it stood on, to climb the pillar beside it,
// burned a dozen walk-over lines); for too little headroom, a line that
// lowers a floor or raises a ceiling on either side; for a gap too narrow,
// one that does so to a sector narrowing it (bars that lower), or a door
// narrowing it, opened from its face, the one the player can open soonest
// (OpeningCost); or else a line that lowers a floor or raises a ceiling on
// either side (a lift the player rides down to where the way on has
// headroom, Doom II MAP05). One sector at a time: if the gap is still too
// narrow once it moved, the next one is pushed (Doom II MAP19: three key
// bars, two of which must be open). For a jump that does not work now, a
// line that raises its takeoff floor back, or lowers its landing. Where no
// line does it, the map's bosses whose death does (BossFor). want gets the
// moves that would help.
static boolean FixFor(int portal, int from, k_crossing_t crossing, goal_type_t *type,
                      int *param, int *want)
{
    const k_map_t *map = K_Map_Get();
    const k_portal_t *p = &K_Graph_Get()->portals[portal];
    int to = p->cell[0] == from ? p->cell[1] : p->cell[0];
    int from_sector = map->subsectors[map->cells[from].subsector].sector;
    int to_sector = map->subsectors[map->cells[to].subsector].sector;
    int sectors_[8];
    int wants[8];
    int count = 2;
    int barrel, i;

    *want = 0;

    // A barrel standing in the way, or narrowing it: shot (Doom II MAP23).
    if ((crossing == K_CROSS_THING || crossing == K_CROSS_NARROW)
     && (barrel = K_Graph_BarrelInTheWay(portal, from)) >= 0 && Allowed(GOAL_BARREL, barrel))
    {
        *type = GOAL_BARREL;
        *param = barrel;
        return true;
    }

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
        int narrowing = K_Graph_NarrowSectors(portal, from, sectors_, arrlen(sectors_));
        int best = OpenSoonest(sectors_, narrowing);

        if (best >= 0)
        {
            *type = GOAL_TRIGGER;
            *param = sectors_[best];
            *want = OPENING_MOVES | K_MOVE_DOOR;
            return true;
        }

        sectors_[0] = from_sector;
        wants[0] = OPENING_MOVES;
        sectors_[1] = to_sector;
        wants[1] = OPENING_MOVES;
    }
    else if (crossing == K_CROSS_STEP)
    {
        sectors_[0] = to_sector;
        wants[0] = K_MOVE_FLOOR_DOWN;
        sectors_[1] = from_sector;
        wants[1] = K_MOVE_FLOOR_UP;
    }
    else if (crossing == K_CROSS_HEADROOM)
    {
        sectors_[0] = to_sector;
        wants[0] = OPENING_MOVES;
        sectors_[1] = from_sector;
        wants[1] = OPENING_MOVES;
    }
    else if (crossing == K_CROSS_GAP && p->jump >= 0)
    {
        // A jump found as if a sector had moved: move it there (Doom II
        // MAP09: switch 126 raises the floor a jump lands on, switch 157
        // opens the door another starts from). One found with a door open
        // that is closed: open it (MAP09: switch 455 opens the door at the
        // top of the stairs). A jump that worked when it was found: its
        // takeoff floor has gone down since, or its landing up (Doom II
        // MAP08: walking over line 599 lowers the floor the jump back over
        // the slime starts from, and switch 572 raises it again).
        const k_jump_t *jump = K_Jump_Get(p->jump);
        double floor, ceiling;

        count = 0;

        if (jump->as_if_sector >= 0)
        {
            K_Map_SectorHeights(jump->as_if_sector, &floor, &ceiling);

            if (floor < jump->as_if_floor - 0.5)
            {
                sectors_[count] = jump->as_if_sector;
                wants[count++] = K_MOVE_FLOOR_UP;
            }
            else if (floor > jump->as_if_floor + 0.5)
            {
                sectors_[count] = jump->as_if_sector;
                wants[count++] = K_MOVE_FLOOR_DOWN;
            }

            if (ceiling < jump->as_if_ceiling - 0.5)
            {
                sectors_[count] = jump->as_if_sector;
                wants[count++] = K_MOVE_CEILING_UP;
            }
        }

        if (jump->door >= 0)
        {
            K_Map_SectorHeights(jump->door, &floor, &ceiling);

            if (ceiling - floor < PLAYER_HEIGHT)
            {
                sectors_[count] = jump->door;
                wants[count++] = K_MOVE_CEILING_UP;
            }
        }

        K_Map_SectorHeights(from_sector, &floor, &ceiling);

        if (floor < jump->from_floor - 0.5)
        {
            sectors_[count] = from_sector;
            wants[count++] = K_MOVE_FLOOR_UP;
        }

        K_Map_SectorHeights(jump->land_sector, &floor, &ceiling);

        if (floor > jump->land_floor + 0.5)
        {
            sectors_[count] = jump->land_sector;
            wants[count++] = K_MOVE_FLOOR_DOWN;
        }
    }
    else
    {
        return false;
    }

    for (i = 0; i < count; ++i)
    {
        if (sectors_[i] >= 0 && LiveTrigger(sectors_[i], wants[i])
         && Allowed(GOAL_TRIGGER, sectors_[i]))
        {
            *type = GOAL_TRIGGER;
            *param = sectors_[i];
            *want = wants[i];
            return true;
        }
    }

    for (i = 0; i < count; ++i)
    {
        int boss = sectors_[i] >= 0 ? BossFor(sectors_[i], wants[i]) : -1;

        if (boss >= 0)
        {
            *type = GOAL_KILL;
            *param = boss;
            return true;
        }
    }

    return false;
}

// Whether a route crosses a door that needs a key the player lacks.
static boolean LockedOnRoute(const k_path_t *path)
{
    int i;

    for (i = 0; i < path->num_cells - 1; ++i)
    {
        if (K_Graph_CanCross(path->portals[i], path->cells[i]) == K_CROSS_LOCKED)
        {
            return true;
        }
    }

    return false;
}

// Whether the player can get to work a trigger fix without a key it lacks:
// some line it can work, with a route there, open or blocked, through no
// door that needs a key it lacks (Doom II MAP14: the fix further on was a
// switch behind the red door the bot was fetching the key for).
static boolean FixWithoutKey(goal_type_t type, int param, int want)
{
    static candidate_t candidates[MAX_CANDIDATES];
    goal_t goal;
    k_pose_t player;
    k_vec2_t start;
    int count, i, door;

    if (type != GOAL_TRIGGER || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    memset(&goal, 0, sizeof(goal));
    goal.type = type;
    goal.param = param;
    goal.want = want;
    count = Candidates(&goal, candidates, MAX_CANDIDATES);
    start.x = player.x;
    start.y = player.y;

    for (i = 0; i < count; ++i)
    {
        int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));

        if ((key >= 0 && !K_Specials_HasKey(key))
         || (candidates[i].kind == K_GOAL_USE_LINE && DoorKeyInTheWay(&candidates[i], &door) >= 0))
        {
            continue;
        }

        K_Path_Find(&probe, start, player.cell, candidates[i].at);

        if (probe.result == K_PATH_FOUND
         || (probe.result == K_PATH_BLOCKED && !LockedOnRoute(&probe)))
        {
            return true;
        }
    }

    return false;
}

// Whether what a trigger fix does lasts only a while: a lift comes back up,
// and a door that only lines closing it again open shuts (Doom II MAP04:
// door 32, open 4 s).
static boolean FixPasses(goal_type_t type, int sector, int want)
{
    boolean any = false;
    int i;

    if (type != GOAL_TRIGGER)
    {
        return false;
    }

    if (K_Graph_IsLift(sector))
    {
        return true;
    }

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        int line = K_Specials_TriggerLine(sector, i);

        if (!LineHelps(line, want))
        {
            continue;
        }

        if (!K_Specials_DoorCloses(K_Specials_LineSpecial(line)))
        {
            return false;
        }

        any = true;
    }

    return any;
}

// The key a trigger fix needs, if every line that would do it needs one the
// player lacks and can get (Doom II MAP04: door 302, opened by switches for
// the blue key), else -1.
static int FixKey(goal_type_t type, int sector, int want)
{
    int key = -1;
    int i;

    if (type != GOAL_TRIGGER)
    {
        return -1;
    }

    for (i = 0; i < K_Specials_TriggerCount(sector); ++i)
    {
        int line = K_Specials_TriggerLine(sector, i);
        int needs = K_Specials_LockKey(K_Specials_LineSpecial(line));

        if (!LineHelps(line, want))
        {
            continue;
        }

        if (needs < 0 || K_Specials_HasKey(needs) || (key >= 0 && needs != key))
        {
            return -1;
        }

        key = needs;
    }

    return key >= 0 && keys_lying[key] > 0 && Allowed(GOAL_KEY, key) ? key : -1;
}

// The fix for a blocked route: for its first blocked crossing, or none.
// But a lift comes back up, and some doors close again: if that fix works
// one, and a crossing further on is fixed some other way, that one first,
// so the lift is not back up by the time the player returns to it (Doom II
// MAP09: lift 123 lowered, then door 130 opened again further on, and the
// lift worked a second time), nor the door shut (MAP04: door 32 opened, then
// floor 84 lowered back the way the bot came). And a key comes first,
// wherever its door is on the route, or a door opened only by switches
// that need it: keys stay, while what a switch does may not last until the
// player gets there (a lift goes back up while it fetches the key, Doom II
// MAP12). Sets portal and from to the crossing fixed, later if it is not the
// first one, and key_door to that door's portal, else -1.
static boolean RouteFix(const k_path_t *path, goal_type_t *type, int *param, int *want,
                        int *portal, int *from, boolean *later, int *key_door)
{
    goal_type_t key_type, later_type;
    int key_param, key_want, later_param, later_want;
    int i;

    *key_door = -1;
    *portal = path->blocked_portal;
    *from = path->blocked_from;
    *later = false;

    // A gap inside a cell is no crossing's: no portal, but the cell (and the
    // path's blocked_at).
    if (path->blocked_pinch)
    {
        *portal = -1;

        if (!FixForPinch(path->blocked_from, path->blocked_at, type, param, want))
        {
            return false;
        }
    }
    else if (!FixFor(path->blocked_portal, path->blocked_from, path->blocked_by, type, param,
                     want))
    {
        return false;
    }

    // A crossing further on that nothing fixes now gets the fixes before it
    // nowhere (Doom II MAP14: the switch for a sector narrowing the way was
    // behind the red door, whose key the bot was out fetching; it opened
    // the doors in front, closing again, over and over).
    for (i = 0; i < path->num_cells - 1; ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (!K_Graph_Passable(crossing)
         && !FixFor(path->portals[i], path->cells[i], crossing, &later_type, &later_param,
                    &later_want))
        {
            return false;
        }
    }

    for (i = 0; i < path->num_cells - 1 && FixPasses(*type, *param, *want); ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (path->portals[i] != path->blocked_portal && !K_Graph_Passable(crossing)
         && FixFor(path->portals[i], path->cells[i], crossing, &later_type, &later_param,
                   &later_want)
         && !FixPasses(later_type, later_param, later_want)
         && FixWithoutKey(later_type, later_param, later_want))
        {
            *type = later_type;
            *param = later_param;
            *want = later_want;
            *portal = path->portals[i];
            *from = path->cells[i];
            *later = true;
        }
    }

    for (i = 0; i < path->num_cells - 1 && *type != GOAL_KEY; ++i)
    {
        k_crossing_t crossing = K_Graph_CanCross(path->portals[i], path->cells[i]);

        if (crossing == K_CROSS_LOCKED
         && FixFor(path->portals[i], path->cells[i], K_CROSS_LOCKED, &key_type, &key_param,
                   &key_want))
        {
            *type = key_type;
            *param = key_param;
            *want = key_want;
            *key_door = path->portals[i];
        }
        else if (!K_Graph_Passable(crossing)
              && FixFor(path->portals[i], path->cells[i], crossing, &later_type, &later_param,
                        &later_want)
              && (key_param = FixKey(later_type, later_param, later_want)) >= 0)
        {
            *type = GOAL_KEY;
            *param = key_param;
            *want = 0;
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
    int param, want;

    return FixFor(portal, from, crossing, &type, &param, &want) ? FIXABLE_COST
                                                                : UNFIXABLE_COST;
}

// Set as the pathfinder's cost for a gap inside a cell.
static double PinchCost(int cell, k_vec2_t at)
{
    goal_type_t type;
    int param, want;

    return FixForPinch(cell, at, &type, &param, &want) ? FIXABLE_COST : UNFIXABLE_COST;
}

static void CountKeys(k_vec2_t from)
{
    k_keything_t keys[MAX_CANDIDATES];
    int key, i;

    planned_from = from;

    for (key = 0; key < K_NUM_KEYS; ++key)
    {
        keys_lying[key] = K_Specials_FindKeys(key, keys, MAX_CANDIDATES);

        for (i = 0; i < SDL_min(keys_lying[key], MAX_CANDIDATES); ++i)
        {
            keys_at[key][i] = keys[i].at;
        }
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
    goal->changed = false;
    goal->wait = 0;
    goal->waited = 0;
    goal->idle = false;
    goal->shooting = false;

    if (goal->type == GOAL_TRIGGER)
    {
        K_Map_SectorHeights(goal->param, &goal->floor, &goal->ceiling);
        goal->settling = K_Graph_SectorBusy(goal->param);

        if (line_uses != NULL && candidate->line >= 0)
        {
            line_uses[candidate->line] = LineUses(candidate->line) + 1;
            line_last[candidate->line] = leveltime;
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

// Whether some candidate of a goal is open to the player now: one it can
// work (no key it lacks, none needed for a door in front of a switch), with
// an open route there.
static boolean HasOpenWay(const goal_t *goal, const k_pose_t *player)
{
    static candidate_t candidates[MAX_CANDIDATES];
    k_vec2_t start = { player->x, player->y };
    int count = Candidates(goal, candidates, MAX_CANDIDATES);
    int i, door;

    for (i = 0; i < count; ++i)
    {
        if (WasTried(goal, candidates[i].at))
        {
            continue;
        }

        if (candidates[i].line >= 0)
        {
            int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));

            if ((key >= 0 && !K_Specials_HasKey(key))
             || (candidates[i].kind == K_GOAL_USE_LINE && DoorKeyInTheWay(&candidates[i], &door) >= 0))
            {
                continue;
            }
        }

        K_Path_Find(&scratch, start, player->cell, candidates[i].at);

        if (scratch.result == K_PATH_FOUND)
        {
            return true;
        }
    }

    return false;
}

// Before the goal on top is planned: if a goal further down has an open way
// now, the goals above it were pushed for a way that is no longer needed,
// and are dropped (Doom II MAP14: switch 416 opened the door on the way to
// the red key, while the goal on top was still a switch for another way
// round, and from there the bot looped). The lowest such goal wins. An item
// is a detour, not a fix for the goals below it, which have their way: only
// the item and the fixes pushed for it count (Doom II MAP04: the switch for
// the bars before a shotgun was dropped with the shotgun, every second).
// Returns true if goals were dropped.
static boolean DropNeedless(void)
{
    k_pose_t player;
    char name[64];
    int base = 0;
    int i;

    for (i = depth - 1; i >= 0; --i)
    {
        if (goals[i].type == GOAL_ITEM)
        {
            base = i;
            break;
        }
    }

    if (depth < 2 || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    for (i = base; i < depth - 1; ++i)
    {
        if (goals[i].type == GOAL_GOTO || !HasOpenWay(&goals[i], &player))
        {
            continue;
        }

        GoalName(&goals[i], name, sizeof(name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: there is an open way now - dropping the %d "
              "goals above it", name, depth - 1 - i);
        depth = i + 1;
        goals[i].planned = false;
        K_Nav_Clear();
        return true;
    }

    return false;
}

static void PlanGoal(goal_t *goal)
{
    static candidate_t candidates[MAX_CANDIDATES];
    int count = Candidates(goal, candidates, MAX_CANDIDATES);
    int best_open = -1, best_blocked = -1, reachable = 0;
    int waitable = 0;   // candidates with no way now, which a move may open
    int moving, boss;
    double open_length = 0.0, blocked_score = 0.0;
    goal_type_t fix_type = GOAL_GOTO;
    int fix_param = 0, fix_want = 0, fix_portal = -1, fix_from = -1;
    k_vec2_t fix_pinch_at = { 0.0, 0.0 };
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
    CountKeys(start);
    GoalName(goal, name, sizeof(name));
    num_reasons = 0;

    M_snprintf(reason, sizeof(reason), "%s",
               goal->type == GOAL_EXIT ? "the map has no exit line or floor"
             : goal->type == GOAL_KEY ? "no such key lies in the level"
             : goal->type == GOAL_TRIGGER ? "no line moves the sector any more"
             : goal->type == GOAL_ITEM ? "it is gone, or no spot touches it"
             : goal->type == GOAL_BARREL ? "no safe spot to shoot it from"
             : "no way there");

    for (i = 0; i < count; ++i)
    {
        char text[K_LOG_LINE_LEN];
        goal_type_t type;
        int param, want, portal, from, key_door;
        boolean later;

        if (WasTried(goal, candidates[i].at))
        {
            M_snprintf(reason, sizeof(reason), "the %s did not work out",
                       candidates[i].what);
            KeepReason(reason);
            continue;
        }

        K_Path_Find(&scratch, start, player.cell, candidates[i].at);
        K_Path_Describe(&scratch, text, sizeof(text));

        // A switch that needs a key (red bars, say), or stands behind a door
        // that does: nothing happens until the player has it.
        if (candidates[i].line >= 0)
        {
            int key = K_Specials_LockKey(K_Specials_LineSpecial(candidates[i].line));
            int door = -1;
            char needs[64];

            if ((key < 0 || K_Specials_HasKey(key))
             && candidates[i].kind == K_GOAL_USE_LINE)
            {
                key = DoorKeyInTheWay(&candidates[i], &door);
            }

            if (key >= 0 && !K_Specials_HasKey(key))
            {
                if (door >= 0)
                {
                    M_snprintf(needs, sizeof(needs), "is behind a door at line %d that "
                               "needs the %s key", door, K_Specials_KeyName(key));
                }
                else
                {
                    M_snprintf(needs, sizeof(needs), "needs the %s key",
                               K_Specials_KeyName(key));
                }

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
                        fix_want = 0;
                        M_snprintf(blocker, sizeof(blocker), "the %s %s",
                                   candidates[i].what, needs);
                    }
                }
                else
                {
                    M_snprintf(reason, sizeof(reason), "the %s %s, and there is none to "
                               "get", candidates[i].what, needs);
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
              && RouteFix(&scratch, &type, &param, &want, &portal, &from, &later, &key_door))
        {
            double score = scratch.blocked_count * FIXABLE_COST + scratch.length;

            if (best_blocked < 0 || score < blocked_score)
            {
                best_blocked = i;
                blocked_score = score;
                fix_type = type;
                fix_param = param;
                fix_want = want;
                fix_portal = portal;
                fix_from = from;
                fix_pinch_at = scratch.blocked_at;

                if (key_door >= 0)
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by %s, and "
                               "further on by a door that needs the %s key - the key "
                               "first", candidates[i].what, text,
                               K_Specials_KeyName(param));
                }
                else if (later)
                {
                    M_snprintf(blocker, sizeof(blocker), "the %s is blocked by a lift or a "
                               "door that shuts again (%s) - first the crossing further on "
                               "(%s)", candidates[i].what, text,
                               K_Graph_CrossingName(K_Graph_CanCross(portal, from)));
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
    else if (goal->type == GOAL_ITEM
          && (!goal->fixable || best_blocked < 0 || fix_type == GOAL_KEY))
    {
        // A detour, not a step on the way: never worth a wait, nor a fix
        // unless it was gone for through one (FIX_WORK), and never a key.
        FailGoal(reason);
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
            fix->want = fix_want;

            if (fix_portal < 0)
            {
                fix->pinch_cell = fix_from;
                fix->pinch_at = fix_pinch_at;
            }
        }

        GoalName(fix, fix_name, sizeof(fix_name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s - added %s", name, blocker, fix_name);
    }
    else if (goal->type == GOAL_EXIT && (boss = BossFor(-1, 0)) >= 0)
    {
        // The level ends when the last of the map's bosses dies (E2M8,
        // E3M8: no exit line).
        goal_t *fix;
        char fix_name[64];

        M_snprintf(goal->note, sizeof(goal->note), "the level ends when the last %s dies",
                   K_Combat_KindName(boss));
        fix = Push(GOAL_KILL, boss);
        GoalName(fix, fix_name, sizeof(fix_name));
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: %s - added %s", name, goal->note, fix_name);
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
    else if (goal->type == GOAL_EXIT)
    {
        // Never given up: the goals that failed on the way come back after
        // a while, and something may open a way meanwhile.
        if (!goal->idle)
        {
            LogReasons(name);
            K_Log(K_LOG_BRAIN, K_LOG_WARN, "%s: no way for now - %s; waiting, planning again "
                  "every %d s", name, reason, IDLE_TICS / TICRATE);
            goal->idle = true;
        }

        M_snprintf(goal->note, sizeof(goal->note), "no way for now, waiting - %s", reason);
        goal->retry_tic = leveltime + IDLE_TICS;
        goal->waited = 0;
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

    if (goal->type == GOAL_TRIGGER && goal->pinch_cell >= 0)
    {
        if (K_Graph_PinchNarrow(goal->pinch_cell, goal->pinch_at))
        {
            return false;
        }

        DoneGoal("the gap is wide enough now, or widening");
        return true;
    }

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

    // Busy when its line was gone for: what it does until it stops is not
    // the line's doing, and nav waits at the switch meanwhile (Doom II
    // MAP02: switch 102, gone for as door 30 closed by itself, counted as
    // having moved it, twice in 0.1 s, then once more, using it up).
    if (goal->settling)
    {
        if (K_Graph_SectorBusy(goal->param))
        {
            return false;
        }

        goal->settling = false;
        K_Map_SectorHeights(goal->param, &goal->floor, &goal->ceiling);
    }

    K_Map_SectorHeights(goal->param, &floor, &ceiling);

    if (floor != goal->floor || ceiling != goal->ceiling)
    {
        goal->changed = true;
    }

    if (!goal->moving)
    {
        if (goal->changed || mover != K_MOVER_NONE)
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
        // A mover set going can have nowhere to go: a lift already at the
        // bottom waits there at once (Doom II MAP28), and counting that as
        // moved, the brain worked its switches round and round.
        if (!goal->changed)
        {
            CandidateFailed(goal, "using it moved nothing");
            return true;
        }

        M_snprintf(how, sizeof(how), "sector %d moved (floor %.0f, ceiling %.0f)",
                   goal->param, floor, ceiling);
        DoneGoal(how);
    }

    return true;
}

static boolean OnExitFloor(void)
{
    const mobj_t *mo = players[consoleplayer].mo;

    return mo != NULL && K_Specials_ExitSector((int) (mo->subsector->sector - sectors));
}

// Gone for the floor that ends the level (E1M8): once on it, the player
// stays put, whatever its health (no detour: ConsiderItems), until the
// floor has taken it down to 10 and the level ends; pushed off, it goes back.
// Returns true while it handles the goal.
static boolean UpdateExitFloor(goal_t *goal)
{
    if (goal->chosen.line >= 0)
    {
        return false;
    }

    if (!OnExitFloor())
    {
        if (goal->staying)
        {
            goal->staying = false;
            goal->planned = false;
            K_Nav_Clear();
            return true;
        }

        return false;
    }

    if (!goal->staying)
    {
        K_Log(K_LOG_BRAIN, K_LOG_INFO, "GO TO EXIT: on the exit floor - staying while it takes "
              "the health down to 10, which ends the level");
        M_StringCopy(goal->note, "on the exit floor, waiting for it to end the level",
                     sizeof(goal->note));
        goal->staying = true;
        K_Nav_Clear();
    }

    return true;
}

// The monster of the kind gone for has moved KILL_DRIFT from where it stood
// then (looked at every KILL_LOOK_TICS, not in a fight): planned again.
// Returns true if it was.
#define KILL_DRIFT 256.0
#define KILL_LOOK_TICS TICRATE

static boolean UpdateKill(goal_t *goal)
{
    k_vec2_t at[MAX_CANDIDATES];
    double nearest = -1.0;
    int count, i;

    if (++goal->wait < KILL_LOOK_TICS || K_Combat_GetStatus()->battle)
    {
        return false;
    }

    goal->wait = 0;
    count = SDL_min(K_Specials_FindMonsters(goal->param, at, MAX_CANDIDATES), MAX_CANDIDATES);

    for (i = 0; i < count; ++i)
    {
        double distance = hypot(at[i].x - goal->chosen.at.x, at[i].y - goal->chosen.at.y);

        if (nearest < 0.0 || distance < nearest)
        {
            nearest = distance;
        }
    }

    if (nearest >= 0.0 && nearest < KILL_DRIFT)
    {
        return false;
    }

    goal->planned = false;
    K_Nav_Clear();
    return true;
}

// Whether the item goes before the others (FIRST_SCORE): one worth leaving a
// fight for however far (k_items.h: escape) - but when CRITICAL only health
// (Doom II MAP15: at 9 health a clip for the chaingun, firepower LOW, was
// added over the medikits and dropped for them every second) - and but when
// CRITICAL a weapon at a pistol start.
static boolean GoesFirst(const k_item_t *item, boolean critical)
{
    return (item->escape && (item->kind == K_ITEM_HEALTH || !critical))
        || (item->first && !critical);
}

// Whether something worth leaving a fight for (k_items.h: escape) lies in
// reach: health, when LOW or CRITICAL; but for health CRITICAL, ammo when
// ammo or firepower is LOW. *health tells which.
static boolean EscapeFor(boolean critical, boolean *health)
{
    boolean ammo = false;
    int i;

    *health = false;

    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);

        if (item->escape && item->present && item->reach > 0.0)
        {
            *health |= item->kind == K_ITEM_HEALTH;
            ammo |= item->kind != K_ITEM_HEALTH && !critical;
        }
    }

    return *health || ammo;
}

// How far out of its way the player may go for an item now: in a fight, a
// dash at most, but for one worth leaving the fight for (escape).
static double ItemReach(const k_item_t *item, boolean fighting)
{
    if (fighting && !item->escape)
    {
        return SDL_min(item->reach, FIGHT_REACH);
    }

    return item->reach;
}

// How far a point lies from the route ahead, within ROUTE_AHEAD along it
// from the player (start), the next waypoint `from`.
static double RouteDistance(const k_path_t *path, int from, k_vec2_t start, k_vec2_t at)
{
    double best = 1.0e9, left = ROUTE_AHEAD;
    k_vec2_t a = start;
    int k;

    for (k = SDL_max(from, 0); k < path->num_points && left > 0.0; ++k)
    {
        k_vec2_t b = path->points[k];
        double dx = b.x - a.x, dy = b.y - a.y;
        double length = sqrt(dx * dx + dy * dy);
        double t = length > 0.0 ? ((at.x - a.x) * dx + (at.y - a.y) * dy) / (length * length)
                                : 0.0;
        double ox, oy;

        t = SDL_max(0.0, SDL_min(1.0, t));
        ox = a.x + dx * t - at.x;
        oy = a.y + dy * t - at.y;
        best = SDL_min(best, sqrt(ox * ox + oy * oy));
        left -= length;
        a = b;
    }

    return best;
}

// A route that is open, or (fixed) blocked where the fix of that type and
// param opens it: the fix for it (RouteFix) is that one. However many
// crossings it is: the bars of one sector are a crossing each (Doom II
// MAP04: twelve, the shotguns behind them left).
static boolean OpenOrFixed(const k_path_t *path, boolean fixed, goal_type_t type, int param)
{
    goal_type_t fix_type;
    int fix_param, want, portal, from, key_door;
    boolean later;

    return path->result == K_PATH_FOUND
        || (fixed && path->result == K_PATH_BLOCKED
            && RouteFix(path, &fix_type, &fix_param, &want, &portal, &from, &later, &key_door)
            && fix_type == type && fix_param == param);
}

// Marks the areas the route goes through from its route cell `from` on
// (route_marks).
static void MarkRouteAreas(const k_path_t *path, int from)
{
    int i;

    if (marks_size < K_Area_Get()->num_areas)
    {
        marks_size = K_Area_Get()->num_areas * 2;
        route_marks = realloc(route_marks, marks_size * sizeof(*route_marks));
        risk_marks = realloc(risk_marks, marks_size * sizeof(*risk_marks));
        memset(route_marks, 0, marks_size * sizeof(*route_marks));
        memset(risk_marks, 0, marks_size * sizeof(*risk_marks));
    }

    ++route_stamp;

    for (i = SDL_max(from, 0); path != NULL && i < path->num_cells; ++i)
    {
        int area = K_Area_Of(path->cells[i]);

        if (area >= 0)
        {
            route_marks[area] = route_stamp;
        }
    }
}

// What the monsters inside the areas a way goes through would cost (k_area.h:
// inside), but for the areas of the route (MarkRouteAreas), the area `skip`
// and those counted already for this detour (risk_marks).
static double WayRisk(const k_path_t *path, int skip)
{
    double risk = 0.0;
    int i;

    for (i = 0; i < path->num_cells; ++i)
    {
        int area = K_Area_Of(path->cells[i]);

        if (area < 0 || area >= marks_size || area == skip || route_marks[area] == route_stamp
         || risk_marks[area] == risk_stamp)
        {
            continue;
        }

        risk_marks[area] = risk_stamp;
        risk += K_Area_Get()->areas[area].inside;
    }

    return risk;
}

// The extra way an item takes: there and on to where the route goes, less
// the way left; the way there (the work at a fix counted); and the health
// the floors take there and on, less what they take on the way left. Ways
// with their waits (WAIT_WAY). With fixes allowed, a way there blocked by
// one fix the brain knows (FIX_WORK) is counted through it, and fixed set.
// risk, if not NULL, gets what the monsters in the areas the detour goes
// through and the route does not would cost (WayRisk; not on the way to a
// fix). Returns false if there is no way there, or none on.
static boolean ItemDetour(k_vec2_t start, int cell, k_vec2_t spot, k_vec2_t end,
                          double remaining, double remaining_damage, boolean fixes,
                          double *detour, double *way, double *damage, boolean *fixed,
                          double *risk)
{
    goal_type_t type = GOAL_GOTO;
    int param = -1, want, portal, from, key_door, count, i;
    int spot_area = K_Area_Of(K_Map_CellAt(spot.x, spot.y));
    boolean later;
    double there = -1.0, there_damage = 0.0, there_risk = 0.0;
    goal_t fix;

    *fixed = false;
    ++risk_stamp;
    K_Path_Find(&probe, start, cell, spot);

    if (probe.result == K_PATH_FOUND)
    {
        if (probe.wait > ITEM_WAIT_MOST)
        {
            return false;
        }

        there = probe.length + probe.wait * WAIT_WAY;
        there_damage = probe.damage;
        there_risk = WayRisk(&probe, spot_area);
    }
    else if (fixes && probe.result == K_PATH_BLOCKED
          && RouteFix(&probe, &type, &param, &want, &portal, &from, &later, &key_door)
          && (type == GOAL_BARREL || type == GOAL_TRIGGER) && key_door < 0 && !later)
    {
        // The fix as PlanGoal would push it, and its nearest spot.
        memset(&fix, 0, sizeof(fix));
        fix.type = type;
        fix.param = param;
        fix.want = want;
        fix.portal = portal;
        fix.from = from;
        fix.pinch_cell = -1;

        if (type == GOAL_TRIGGER && portal < 0)
        {
            fix.pinch_cell = from;
            fix.pinch_at = probe.blocked_at;
        }

        count = Candidates(&fix, fix_candidates, MAX_CANDIDATES);

        for (i = 0; i < count; ++i)
        {
            double to_fix, to_fix_damage, to_fix_wait;

            K_Path_Find(&fix_probe, start, cell, fix_candidates[i].at);

            if (fix_probe.result != K_PATH_FOUND)
            {
                continue;
            }

            to_fix = fix_probe.length + fix_probe.wait * WAIT_WAY;
            to_fix_damage = fix_probe.damage;
            to_fix_wait = fix_probe.wait;
            K_Path_Find(&fix_probe, fix_candidates[i].at, -1, spot);

            if (OpenOrFixed(&fix_probe, true, type, param)
             && to_fix_wait + fix_probe.wait <= ITEM_WAIT_MOST
             && (there < 0.0
                 || to_fix + fix_probe.length + fix_probe.wait * WAIT_WAY + FIX_WORK < there))
            {
                there = to_fix + fix_probe.length + fix_probe.wait * WAIT_WAY + FIX_WORK;
                there_damage = to_fix_damage + fix_probe.damage;
            }
        }

        *fixed = there >= 0.0;
    }

    if (there < 0.0)
    {
        return false;
    }

    K_Path_Find(&probe, spot, -1, end);

    if (!OpenOrFixed(&probe, *fixed, type, param))
    {
        return false;
    }

    *detour = SDL_max(0.0, there + probe.length + probe.wait * WAIT_WAY - remaining);
    *way = there;
    *damage = SDL_max(0.0, there_damage + probe.damage - remaining_damage);

    if (risk != NULL)
    {
        *risk = there_risk + WayRisk(&probe, spot_area);
    }

    return true;
}

// Going on by a radiation suit: what the way there costs, and the way on
// from it with a fresh suit worn, the waits and the floors counted
// (WAIT_WAY, DAMAGE_WAY); the way there; and the extra way, less the way
// left (remaining). Returns false if there is no open way there, or none
// on.
static boolean SuitDetour(k_vec2_t start, int cell, k_vec2_t spot, k_vec2_t end,
                          double remaining, double *cost, double *way, double *detour)
{
    K_Path_Find(&probe, start, cell, spot);

    if (probe.result != K_PATH_FOUND || probe.wait > ITEM_WAIT_MOST)
    {
        return false;
    }

    *way = probe.length + probe.wait * WAIT_WAY;
    *cost = *way + probe.damage * DAMAGE_WAY;
    K_Path_AssumeSuit(IRONTICS);
    K_Path_Find(&probe, spot, -1, end);
    K_Path_AssumeSuit(-1.0);

    if (probe.result != K_PATH_FOUND)
    {
        return false;
    }


    *cost += probe.length + probe.wait * WAIT_WAY + probe.damage * DAMAGE_WAY;
    *detour = SDL_max(0.0, *way + probe.length + probe.wait * WAIT_WAY - remaining);
    return true;
}

// Pickups (plan, 7.5): an item worth a detour - one whose extra way, there
// and on to where the route goes, is within how far out of its way the
// player may go for it now (k_items.h) - goes on top of the chain, and
// when it is picked up the goal below is planned again from there. Looked
// for only while the player follows a route to the goal on top: not while
// working a door, lift or switch, running a jump, going where a click sent
// it, or waiting for a switch's sector. In a fight, only what is worth
// breaking it off for (k_items.h: urgent), within FIGHT_REACH but for
// health when CRITICAL (UpdateEscape). Out of a fight, an item behind one
// fix is gone for through it (FIX_WORK). An item the player could not get
// on from (down in a pit, past a one-way drop) is not gone for. Returns
// true if one was added.
static boolean ConsiderItems(goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_path_t *path = K_Nav_GetPath();
    boolean critical = K_Condition_Get()->health == K_NEED_CRITICAL;
    boolean fighting = K_Combat_GetStatus()->battle;
    int routes = critical ? MAX_ITEM_ROUTES : ITEM_ROUTES;
    int order[MAX_ITEM_ROUTES];
    double order_score[MAX_ITEM_ROUTES];
    int found = 0, best = -1;
    double best_slack = 0.0, best_detour = 0.0, best_way = 0.0, best_damage = 0.0;
    double best_risk = 0.0;
    double remaining_way = 0.0, remaining_damage = 0.0, remaining_cost = 0.0;
    boolean best_fixed = false, best_first = false, best_suit = false, suits = false;
    const k_item_t *chosen;
    k_pose_t player;
    k_vec2_t start, end;
    goal_t *item_goal;
    char name[64], guarded[64] = "", floors[64] = "", risky[64] = "";
    int detour_at = -1;     // the detour on that goes first, which a nearer one may replace
    int i, k;

    // Health just went CRITICAL: looked at now, not within the second (Doom
    // II MAP02: a demon bit the bot 0.9 s after, before the next look).
    if (critical && !was_critical)
    {
        item_check_tic = 0;
    }

    was_critical = critical;

    // A goal not planned yet (the one below an item just picked up) does not
    // use the look up: it comes once it is. Nothing takes the player off the
    // exit floor (E1M8: health is what it takes).
    if (leveltime < item_check_tic || !goal->planned || goal->staying)
    {
        return false;
    }

    item_check_tic = leveltime + ITEM_CHECK_TICS;

    // One detour at a time: not while one is on, nor its fix in front of it
    // (Doom II MAP04: the shotgun behind bars was added again over the
    // switch pushed for it, every second). But health LOW or CRITICAL goes
    // before any other, while some health worth leaving a fight for is there
    // (k_items.h: escape): that one is dropped, with the fixes pushed for it,
    // and health looked for once the goal below is planned again (MAP10: an
    // armour bonus behind a gun switch kept the bot fighting at 20 health, a
    // medikit in reach, until it died). So does the ammo when ammo or
    // firepower is LOW, but for health CRITICAL (MAP10: out of shells, the
    // shotguns dropped beside the bot were passed for a cell charge 2000
    // off, and the bot fought on with the pistol). When LOW, not one that
    // goes first itself (GoesFirst: a weapon at a pistol start, that ammo);
    // that one gives way only to another that goes first, its way there
    // REPLACE_MARGIN shorter (E2M5: a chaingun behind a walk-over line 2788
    // off was gone for at the start; a shotgun guy then dropped his shotgun
    // beside the bot, never looked at, and the bot fought demons with the
    // pistol).
    for (i = 0; i < depth && detour_at < 0; ++i)
    {
        const k_item_t *on;
        boolean for_health;

        if (goals[i].type != GOAL_ITEM)
        {
            continue;
        }

        on = K_Items_Get(goals[i].param);

        if (i > 0 && GoesFirst(on, critical) && (on->kind == K_ITEM_HEALTH || !critical))
        {
            detour_at = i;
            continue;
        }

        if (i == 0 || on->kind == K_ITEM_HEALTH || (!critical && GoesFirst(on, critical))
         || !EscapeFor(critical, &for_health))
        {
            return false;
        }

        GoalName(&goals[i], name, sizeof(name));

        if (for_health)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - health %s, health goes first", name,
                  critical ? "CRITICAL" : "LOW");
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - %s LOW, ammo goes first", name,
                  K_Condition_Get()->firepower == K_NEED_LOW ? "firepower" : "ammo");
        }

        if (goals[i].param < item_skip_size)
        {
            item_skip[goals[i].param] = leveltime + ITEM_SKIP_TICS;
        }

        depth = i;
        goals[depth - 1].planned = false;
        K_Nav_Clear();
        item_check_tic = 0;
        return true;
    }

    // In a fight nav waits: it was free to when the fight began.
    if (depth >= MAX_GOALS || !goal->planned
     || goal->type == GOAL_GOTO || (goal->type == GOAL_TRIGGER && (goal->used || goal->moving))
     || nav->state != K_NAV_DRIVING || path == NULL || path->num_points < 1
     || (!fighting && (nav->action != K_ACTION_NONE || !K_Nav_CanPause()))
     || !K_Map_GetPlayer(&player))
    {
        return false;
    }

    start.x = player.x;
    start.y = player.y;
    end = path->points[path->num_points - 1];

    if (item_skip_size < K_Items_Count())
    {
        item_skip = realloc(item_skip, K_Items_Count() * 2 * sizeof(*item_skip));
        memset(item_skip + item_skip_size, 0,
               (K_Items_Count() * 2 - item_skip_size) * sizeof(*item_skip));
        item_skip_size = K_Items_Count() * 2;
    }

    // The most promising few, by worth and distance: as the crow flies, or
    // twice that from the route ahead.
    for (i = 0; i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);
        double distance = sqrt((item->at.x - start.x) * (item->at.x - start.x)
                             + (item->at.y - start.y) * (item->at.y - start.y));
        double reach = ItemReach(item, fighting);
        double score;

        suits |= item->suit && item->present;

        if (item->present && reach > 0.0 && distance > reach)
        {
            distance = SDL_min(distance, 2.0 * RouteDistance(path, nav->target, start, item->at));
        }

        if (!item->present || reach <= 0.0 || distance > reach
         || leveltime < item_skip[i] || IsFailed(GOAL_ITEM, i)
         || (fighting && !item->urgent)
         || (detour_at >= 0 && (!GoesFirst(item, critical) || i == goals[detour_at].param)))
        {
            continue;
        }

        score = GoesFirst(item, critical) ? FIRST_SCORE - distance : reach / (distance + 64.0);

        // Kept best first.
        if (found == routes && score <= order_score[routes - 1])
        {
            continue;
        }

        for (k = found < routes ? found++ : routes - 1;
             k > 0 && order_score[k - 1] < score; --k)
        {
            order[k] = order[k - 1];
            order_score[k] = order_score[k - 1];
        }

        order[k] = i;
        order_score[k] = score;
    }

    suits &= !fighting && detour_at < 0 && K_Map_SuitTics() < SUIT_KEEP;

    // The way left as the pathfinder sees it now, with its waits, and the
    // floors that hurt on it; as nav has it if there is none.
    remaining_way = nav->remaining;

    if (found > 0 || suits)
    {
        K_Path_Find(&probe, start, player.cell, end);

        if (probe.result == K_PATH_FOUND)
        {
            remaining_way = probe.length + probe.wait * WAIT_WAY;
            remaining_damage = probe.damage;
            MarkRouteAreas(&probe, 0);
        }
        else
        {
            MarkRouteAreas(path, nav->route_cell);
        }

        remaining_cost = remaining_way + remaining_damage * DAMAGE_WAY;
    }

    for (k = 0; k < found; ++k)
    {
        const k_item_t *item = K_Items_Get(order[k]);
        double reach = ItemReach(item, fighting);
        boolean first = GoesFirst(item, critical);
        double detour, way, damage, risk;
        boolean fixed;

        if (!ItemDetour(start, player.cell, item->spot, end, remaining_way, remaining_damage,
                        !fighting, &detour, &way, &damage, &fixed, &risk))
        {
            item_skip[order[k]] = leveltime + ITEM_SKIP_TICS;
            continue;
        }

        // Health the floors on the way would take most of back: left.
        if (item->kind == K_ITEM_HEALTH && damage > item->together * HEALTH_NET)
        {
            item_skip[order[k]] = leveltime + ITEM_SKIP_TICS;
            continue;
        }

        if (reach > ANY_REACH && risk >= RISKY_ANY && !(critical && item->kind == K_ITEM_HEALTH))
        {
            reach = RISKY_REACH;
        }

        detour += (damage + risk) * DAMAGE_WAY;

        if (detour <= reach
         && (best < 0 || (first && !best_first)
             || (first && best_first && way < best_way)
             || (!first && !best_first && reach - detour > best_slack)))
        {
            best = order[k];
            best_slack = reach - detour;
            best_detour = detour;
            best_way = way;
            best_damage = damage;
            best_risk = risk;
            best_fixed = fixed;
            best_first = first;
        }
    }

    // A radiation suit, by what going on by it saves (SUIT_GAIN); not over a
    // weapon at a pistol start, nor health when CRITICAL.
    for (i = 0; suits && !best_first && i < K_Items_Count(); ++i)
    {
        const k_item_t *item = K_Items_Get(i);
        double cost, way, detour;

        if (!item->suit || !item->present || item->reach <= 0.0 || leveltime < item_skip[i]
         || IsFailed(GOAL_ITEM, i)
         || !SuitDetour(start, player.cell, item->spot, end, remaining_way, &cost, &way, &detour)
         || remaining_cost - cost <= SUIT_GAIN || (best >= 0 && remaining_cost - cost <= best_slack))
        {
            continue;
        }

        best = i;
        best_slack = remaining_cost - cost;
        best_detour = detour;
        best_way = way;
        best_damage = 0.0;
        best_risk = 0.0;
        best_fixed = false;
        best_suit = true;
    }

    if (best < 0)
    {
        return false;
    }

    // In place of the detour on, only when clearly sooner in hand; it and
    // the fixes pushed for it go, the goal below it staying as it was.
    if (detour_at >= 0)
    {
        const k_item_t *on = K_Items_Get(goals[detour_at].param);
        double on_detour, on_way = 1.0e9, on_damage;
        boolean on_fixed;

        ItemDetour(start, player.cell, on->spot, end, remaining_way, remaining_damage, true,
                   &on_detour, &on_way, &on_damage, &on_fixed, NULL);

        if (best_way + REPLACE_MARGIN >= on_way)
        {
            return false;
        }

        GoalName(&goals[detour_at], name, sizeof(name));

        if (on_way < 1.0e9)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - the %s is in hand sooner, %.0f units "
                  "against %.0f", name, K_Items_Get(best)->name, best_way, on_way);
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - the %s is in hand sooner, %.0f units "
                  "(no way from here to the %s)", name, K_Items_Get(best)->name, best_way,
                  on->name);
        }
        depth = detour_at;
        K_Nav_Clear();
    }

    // A switch not worked yet: going for it again after the item is not
    // another use of its line (MAX_TRIGGERS). Already so for one in place
    // of a detour.
    else if (goal->type == GOAL_TRIGGER && line_uses != NULL && goal->chosen.line >= 0
          && line_uses[goal->chosen.line] > 0)
    {
        --line_uses[goal->chosen.line];
    }

    item_goal = Push(GOAL_ITEM, best);
    item_goal->fixable = best_fixed;
    GoalName(item_goal, name, sizeof(name));
    chosen = K_Items_Get(best);

    if (chosen->danger >= 0.5)
    {
        M_snprintf(guarded, sizeof(guarded), ", guarded: %.0f health at risk", chosen->danger);
    }

    if (best_suit)
    {
        M_snprintf(floors, sizeof(floors), ", going on by it saves %.0f in way and floors "
                   "that hurt", best_slack);
    }
    else if (best_damage >= 0.5)
    {
        M_snprintf(floors, sizeof(floors), ", %.0f health off floors that hurt", best_damage);
    }

    if (best_risk >= 0.5)
    {
        M_snprintf(risky, sizeof(risky), ", %.0f health at risk in the areas on the way",
                   best_risk);
    }

    K_Log(K_LOG_BRAIN, K_LOG_INFO, "added %s at (%.0f,%.0f) - %.0f units out of the way%s%s%s, "
          "worth %.0f (%.0f with those beside it)%s", name, chosen->at.x, chosen->at.y,
          best_detour, best_fixed ? " through what blocks it" : "", floors, risky,
          chosen->worth, chosen->together, guarded);
    return true;
}

// Fleeing (plan, 7.4), and dashes: while the goal on top is an item worth
// breaking a fight off for (k_items.h: urgent) - health when LOW or
// CRITICAL, and ammo when ammo or firepower is LOW, wherever it is
// (escape); a weapon the player lacks, or the ammo running out, from when it
// comes within FIGHT_REACH until it is picked up, while no monster is within
// DASH_CLEAR -
// combat starts no fight and breaks off the one going on; the player runs
// for it, shooting on the way at what it sees. The item may have fixes on
// top of it: working them is part of the run for it (MAP10: going for a
// medikit at LOW health, the bot fought an arachnotron by the walk-over line
// pushed for it, and died). With nothing to go for, fights go on as before:
// running to nowhere is no safer, and cover comes with dodging (C3).
static void UpdateEscape(void)
{
    const goal_t *goal = NULL;
    const k_item_t *item = NULL;
    char why[64];
    k_pose_t player;
    int i;

    for (i = depth - 1; i >= 0 && goal == NULL; --i)
    {
        if (goals[i].type == GOAL_ITEM)
        {
            goal = &goals[i];
            item = K_Items_Get(goal->param);
        }
    }

    if (item == NULL || !item->urgent)
    {
        dashing = -1;
        K_Combat_Escape(NULL);
        return;
    }

    if (item->escape)
    {
        M_snprintf(why, sizeof(why), "%s, going for the %s",
                   item->kind != K_ITEM_HEALTH
                   ? (K_Condition_Get()->firepower == K_NEED_LOW ? "firepower LOW" : "ammo LOW")
                   : K_Condition_Get()->health == K_NEED_CRITICAL ? "health CRITICAL"
                   : "health LOW", item->name);
        K_Combat_Escape(why);
        return;
    }

    if (dashing != goal->param && K_Map_GetPlayer(&player)
     && sqrt((item->at.x - player.x) * (item->at.x - player.x)
           + (item->at.y - player.y) * (item->at.y - player.y)) <= FIGHT_REACH)
    {
        dashing = goal->param;
    }

    if (dashing == goal->param && K_Combat_MonstersWithin(DASH_CLEAR) == 0)
    {
        M_snprintf(why, sizeof(why), "going for the %s", item->name);
        K_Combat_Escape(why);
    }
    else
    {
        K_Combat_Escape(NULL);
    }
}

// Held up by a barrel (HOLD_TICS): it goes on top, to be shot from a safe
// spot. Looked at every tic, to time the hold. Returns true if it was
// added.
static boolean ConsiderHeldUp(const goal_t *goal)
{
    const k_navstatus_t *nav = K_Nav_GetStatus();
    const k_map_t *map = K_Map_Get();
    double vx, vy, best_gap = HOLD_GAP, reach = PLAYER_RADIUS + HOLD_GAP + 32.0;
    int found[16], count, best = -1, i;
    k_pose_t player;
    k_vec2_t me, min, max;
    char name[64];

    if (!K_Map_GetPlayer(&player))
    {
        return false;
    }

    me.x = player.x;
    me.y = player.y;

    // Following a route only: not fighting or stepping away (combat moves
    // the player), not at a door or lift, not shooting a barrel already.
    if (!goal->planned || goal->type == GOAL_BARREL || nav->state != K_NAV_DRIVING
     || nav->action != K_ACTION_NONE || K_Combat_Move(&vx, &vy)
     || AxisDistance(me, hold_at) > HOLD_RADIUS)
    {
        hold_at = me;
        hold_since = leveltime;
        return false;
    }

    if (leveltime - hold_since < HOLD_TICS)
    {
        return false;
    }

    hold_at = me;
    hold_since = leveltime;
    min.x = me.x - reach;
    min.y = me.y - reach;
    max.x = me.x + reach;
    max.y = me.y + reach;
    count = K_Map_BlocksNear(min, max, found, arrlen(found));

    for (i = 0; i < count; ++i)
    {
        const k_block_t *block = &map->blocks[found[i]];
        double gap = AxisDistance(me, block->at) - block->radius - PLAYER_RADIUS;

        if (block->barrel && gap < best_gap && Allowed(GOAL_BARREL, found[i]))
        {
            best = found[i];
            best_gap = gap;
        }
    }

    if (best < 0)
    {
        return false;
    }

    GoalName(Push(GOAL_BARREL, best), name, sizeof(name));
    K_Log(K_LOG_BRAIN, K_LOG_INFO, "held up by the barrel at (%.0f,%.0f) for %.1f s - added %s",
          map->blocks[best].at.x, map->blocks[best].at.y, (double) HOLD_TICS / TICRATE, name);
    return true;
}

// At the spot for a barrel in the way, combat shoots it.
static void UpdateShooting(void)
{
    const goal_t *goal = depth > 0 ? &goals[depth - 1] : NULL;

    if (goal != NULL && goal->type == GOAL_BARREL && goal->shooting)
    {
        K_Combat_ShootBarrel(1, goal->chosen.aim.x, goal->chosen.aim.y);
    }
    else
    {
        K_Combat_ShootBarrel(0, 0.0, 0.0);
    }
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
    item_check_tic = 0;
    was_critical = false;
    dashing = -1;
    hold_since = 0;
    hold_at.x = hold_at.y = 1.0e9;

    if (item_skip != NULL)
    {
        memset(item_skip, 0, item_skip_size * sizeof(*item_skip));
    }

    free(line_uses);
    line_uses = calloc(SDL_max(numlines, 1), 1);
    free(line_last);
    line_last = calloc(SDL_max(numlines, 1), sizeof(*line_last));

    K_Path_SetBlockCost(BlockCost);
    K_Path_SetPinchCost(PinchCost);
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
    const k_item_t *item;

    UpdateEscape();
    UpdateShooting();

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

    if ((goal->type == GOAL_KEY && K_Specials_HasKey(goal->param))
     || (goal->type == GOAL_ITEM && !K_Items_Get(goal->param)->present))
    {
        // The next item looked for at once: drops lie together after a fight.
        if (goal->type == GOAL_ITEM)
        {
            item_check_tic = 0;
        }

        DoneGoal("picked it up");
        return;
    }

    // What their death moves is on its way (A_BossDeath runs as the last
    // one dies).
    if (goal->type == GOAL_KILL && K_Specials_FindMonsters(goal->param, NULL, 0) == 0)
    {
        DoneGoal("every one is dead");
        return;
    }

    // Gone, and the chain over: a blast pushes a barrel off its square
    // before it goes off itself (MAP23: the bot set off for the way on, and
    // the barrel in it hit it for 48).
    if (goal->type == GOAL_BARREL && !K_Map_BlockStands(goal->param))
    {
        const k_block_t *barrel = &K_Map_Get()->blocks[goal->param];
        k_pose_t player;

        if (K_Combat_BlastsPending(barrel->at.x, barrel->at.y, BARREL_CHAIN_WAIT) > 0
         || (K_Map_GetPlayer(&player)
             && K_Combat_BlastsPending(player.x, player.y, BARREL_CHAIN_WAIT) > 0))
        {
            M_StringCopy(goal->note, "waiting for the chain to go off", sizeof(goal->note));
            return;
        }

        DoneGoal("it went off");
        return;
    }

    // Something else filled the need on the way (another item, say).
    if (goal->type == GOAL_ITEM && K_Items_Get(goal->param)->worth <= 0.0)
    {
        DoneGoal("no longer needed");
        return;
    }

    // Monsters came, or woke, and guard it now for more than it is worth
    // (or a solid thing came to stand by it): left for a while.
    if (goal->type == GOAL_ITEM && (item = K_Items_Get(goal->param))->reach <= 0.0)
    {
        char name[64];

        GoalName(goal, name, sizeof(name));

        if (!item->reachable)
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - no spot touches it now", name);
        }
        else
        {
            K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s dropped - guarded now, %.0f health at risk "
                  "for %.0f", name, item->danger, item->together);
        }

        if (goal->param < item_skip_size)
        {
            item_skip[goal->param] = leveltime + ITEM_SKIP_TICS;
        }

        Pop();
        return;
    }

    if (DoneIfOpen(goal) || ConsiderHeldUp(goal) || ConsiderItems(goal))
    {
        return;
    }

    if (!goal->planned)
    {
        if (leveltime >= goal->retry_tic && !DropNeedless())
        {
            PlanGoal(goal);
        }

        return;
    }

    if (goal->type == GOAL_TRIGGER && UpdateTrigger(goal))
    {
        return;
    }

    if ((goal->type == GOAL_EXIT && UpdateExitFloor(goal))
     || (goal->type == GOAL_KILL && UpdateKill(goal)))
    {
        return;
    }

    // At the spot, combat shoots; a fight comes first, and does not count.
    if (goal->type == GOAL_BARREL && goal->shooting)
    {
        if (!K_Combat_GetStatus()->battle && ++goal->wait > BARREL_SHOOT_TICS)
        {
            CandidateFailed(goal, "the shots did not set it off");
        }

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
            else if (goal->type == GOAL_KEY || goal->type == GOAL_ITEM)
            {
                k_pose_t pose;

                // Still falling onto it (Doom II MAP03: a box of shells at
                // the foot of a drop): it is picked up on landing.
                if (!K_Map_GetPlayer(&pose) || pose.onground)
                {
                    CandidateFailed(goal, "got there, but did not pick it up");
                }
            }
            else if (goal->type == GOAL_BARREL)
            {
                goal->shooting = true;
                goal->wait = 0;
                M_StringCopy(goal->note, "shooting it", sizeof(goal->note));
            }
            else if (goal->type == GOAL_KILL)
            {
                // It has moved on: where to now, in a moment.
                goal->planned = false;
                goal->retry_tic = leveltime + TICRATE;
                K_Nav_Clear();
            }
            else
            {
                CandidateFailed(goal, "crossed it, but the level did not end");
            }
            break;

        case K_NAV_FAILED:
            // The way shut on the way, but is there, blocked (Doom II
            // MAP02: a fast door closed before the player got back to it,
            // and its only blue key was given up for good): planned again,
            // which pushes the fix (the door's switch), not given up.
            if (K_Nav_GetStatus()->blocked && goal->shut < MAX_SHUT)
            {
                char name[64];

                ++goal->shut;
                GoalName(goal, name, sizeof(name));
                K_Log(K_LOG_BRAIN, K_LOG_INFO, "%s: the way shut on the way (%s) - planning "
                      "again (%d/%d)", name, K_Nav_GetStatus()->problem, goal->shut, MAX_SHUT);
                goal->planned = false;
                K_Nav_Clear();
            }
            else
            {
                CandidateFailed(goal, K_Nav_GetStatus()->problem);
            }
            break;

        default:
            break;
    }
}

void K_Brain_LevelCompleted(int secret)
{
    char how[96];

    M_snprintf(how, sizeof(how), "level finished in %.1f s through the %s exit, %.0f health "
               "left", (double) leveltime / TICRATE, secret ? "secret" : "normal",
               K_Map_PlayerHealth());

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
    static char text[K_LOG_LINE_LEN + 32];
    int left = failed[index].until - leveltime;

    if (left > 0)
    {
        M_snprintf(text, sizeof(text), "%s (again in %d s)", failed[index].text,
                   (left + TICRATE - 1) / TICRATE);
    }
    else
    {
        M_snprintf(text, sizeof(text), "%s (may be tried again)", failed[index].text);
    }

    return text;
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
