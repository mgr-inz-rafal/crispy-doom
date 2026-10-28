//
// DESCRIPTION:
//	Karp's knowledge of what lines and things do: exit lines, lines that
//	move other sectors (switches and walk-over lines), and keys
//	(karp/docs/plan.md, 3.5).
//
//	Plain C types only.
//

#ifndef __K_SPECIALS__
#define __K_SPECIALS__

#include "k_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    K_KEY_BLUE,
    K_KEY_YELLOW,
    K_KEY_RED,
    K_NUM_KEYS
} k_key_t;

// How a line is set off.
typedef enum
{
    K_TRIGGER_NONE,
    K_TRIGGER_USE,          // a switch: press use on its front side
    K_TRIGGER_CROSS,        // a walk-over line: cross it
    K_TRIGGER_SHOOT,        // a gun switch: hit it with a hitscan attack
} k_trigger_t;

// Which way a mover line moves the sectors it targets (flags).
typedef enum
{
    K_MOVE_FLOOR_UP = 1,
    K_MOVE_FLOOR_DOWN = 2,
    K_MOVE_CEILING_UP = 4,
    K_MOVE_DOOR = 8,        // a manual door: its own line opens it
} k_move_t;

// A key lying in the level.
typedef struct
{
    k_vec2_t at;
    int skull;              // a skull key rather than a keycard
} k_keything_t;

// Called on every level load, after the map.
void K_Specials_Build(void);

// Exit lines (11 and 51 are pressed, 52 and 124 crossed); K_TRIGGER_NONE
// for any other special.
k_trigger_t K_Specials_ExitTrigger(int special, int *secret);

// Lines that move the sectors with their tag in a way that can open a way:
// doors that open, floors, lifts, stairs, ceilings that rise; and manual
// doors, which open the sector behind them, whatever the tag. K_TRIGGER_NONE
// for anything else (doors that close, crushers, lights, teleporters).
k_trigger_t K_Specials_MoverTrigger(int special);

// Doors opened by using their own line from the front (EV_VerticalDoor);
// the door is the line's back sector.
int K_Specials_ManualDoor(int special);

// How a mover line moves the sectors it targets, as k_move_t flags (a lift
// goes down, and back up only to where it was; a perpetual one also up to
// the highest neighbouring floor); 0 for any other special.
int K_Specials_MoverMoves(int special);

// A line's live special: one-shot lines lose theirs once used.
int K_Specials_LineSpecial(int line);

// The lines that move a sector (by its tag, or as a manual door's line),
// from the specials at level load.
int K_Specials_TriggerCount(int sector);
int K_Specials_TriggerLine(int sector, int index);

// The key a line special needs (a manual door, or a switch that opens a
// door elsewhere), or -1.
int K_Specials_LockKey(int special);

const char *K_Specials_KeyName(k_key_t key);

// The player holds the keycard or the skull key of that colour.
int K_Specials_HasKey(k_key_t key);

// Keys of a colour lying in the level, cards and skulls; up to max.
// Returns how many there are.
int K_Specials_FindKeys(k_key_t key, k_keything_t *keys, int max);

#ifdef __cplusplus
}
#endif

#endif
