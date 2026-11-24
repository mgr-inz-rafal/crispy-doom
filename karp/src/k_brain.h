//
// DESCRIPTION:
//	Karp priority system (karp/docs/plan.md, 3.5).
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_BRAIN__
#define __K_BRAIN__

#ifdef __cplusplus
extern "C" {
#endif

// Which exit the bot takes (-karpexit): the nearest it can get to, secret or
// not; one kind or the other, drawn for each level; the secret one; the
// normal one. One wanted that stays out of reach gives way to any.
typedef enum
{
    K_EXIT_DEFAULT,
    K_EXIT_RANDOM,
    K_EXIT_SECRET,
    K_EXIT_NORMAL,
} k_exitchoice_t;

void K_Brain_SetExitChoice(k_exitchoice_t choice);

// -karpattitude explorer: places off the way gone to as items are - the
// areas not visited yet, the secrets not found yet - and no level ended on
// the way to something else.
void K_Brain_SetExplorer(int on);

// Back to IDLE, with GO TO EXIT on the list under -autoplay; called on
// every level load.
void K_Brain_Reset(void);

// The level just ended through an exit (secret or not).
void K_Brain_LevelCompleted(int secret);

// A click on the console's map: go to (x, y). Clicks that are not on any
// floor are ignored, with a log line.
void K_Brain_RequestGoto(double x, double y);

// Once per game tic while the bot drives.
void K_Brain_Tick(void);

// One line describing what the bot is doing now, for the console's status
// line.
const char *K_Brain_Status(void);

// What the bot is about, coarsely, as a person watching would put it (the
// overlay, k_overlay.h): what it is after - the purpose - and a fix in its
// way, a switch or barrels, only while that is being worked.
typedef enum
{
    K_ACT_NONE,             // nothing on the list
    K_ACT_EXIT,             // detail: 1 the secret exit, 0 the normal one, -1
                            // whichever
    K_ACT_LOOT,             // the sweep before the exit: the exit, in between
                            // what it fetches
    K_ACT_EXPLORE,          // an explorer's place: an area to visit
    K_ACT_SECRET,           // an explorer's place: a secret to find
    K_ACT_KEY,              // detail: the key (k_key_t)
    K_ACT_TRIGGER,          // a line that opens the way: detail k_opens_t,
                            // flag k_worked_t
    K_ACT_ITEM,             // detail: its k_itemkind_t; name: the item's;
                            // flag 1 if fetched in the sweep before the exit
    K_ACT_ESCAPE,           // an item run for out of a fight (k_items.h:
                            // escape): detail and name as K_ACT_ITEM
    K_ACT_BARREL,           // barrels shot out of the way
    K_ACT_BOSS,             // every monster of a kind dead: detail the kind
                            // (mobjtype_t), name its name, flag 1 if chased
                            // while invulnerable
    K_ACT_TELEFRAG,         // detail and name: the kind landed on
    K_ACT_FALLBACK,         // falling back out of a fight going badly, or
                            // resting where it fell back to
} k_actkind_t;

// K_ACT_TRIGGER: what the line does for the way, by its special at level
// load.
typedef enum
{
    K_OPENS_WAY,            // anything else: a ceiling, a crusher
    K_OPENS_DOOR,
    K_OPENS_LIFT,           // down, and back up after a while
    K_OPENS_FLOOR_UP,       // a floor, a bridge, stairs
    K_OPENS_FLOOR_DOWN,     // a floor that stays down
    K_OPENS_WAY_BACK,       // the way back from where a teleporter brought
                            // the player
} k_opens_t;

// K_ACT_TRIGGER: how the line is worked.
typedef enum
{
    K_WORKED_WALK,          // walked over, or a door's own line used
    K_WORKED_PRESS,         // a switch pressed
    K_WORKED_SHOOT,         // a gun switch shot
} k_worked_t;

typedef struct
{
    // The purpose: the goal worked on, but for a switch or barrels in its
    // way, the one they are for (the exit, if that is all there is) - and a
    // walk somewhere that is no fall back (a way out of a loop, a click), the
    // one under it.
    k_actkind_t kind;
    int detail;
    int flag;
    const char *name;       // or NULL

    // The fix on top being worked now - K_ACT_TRIGGER: its switch pressed or
    // shot, or its sector moving after; K_ACT_BARREL: shot at from its spot
    // - else K_ACT_NONE. Walked to, it is not shown: the purpose is (the
    // user: the bot ran on through corridors, opening doors, under a line
    // about a line 4400 units off).
    k_actkind_t fix;
    int fix_detail;         // as detail and flag for K_ACT_TRIGGER
    int fix_flag;
} k_activity_t;

void K_Brain_Activity(k_activity_t *activity);

// The goal stack, for the console: index 0 is the top, the one worked on.
int K_Brain_GoalCount(void);
void K_Brain_GoalText(int index, char *buf, int buf_len);

// Goals that failed this level, oldest first, and when each may go on the
// stack again.
int K_Brain_FailedCount(void);
const char *K_Brain_FailedText(int index);

#ifdef __cplusplus
}
#endif

#endif
