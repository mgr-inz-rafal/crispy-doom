//
// DESCRIPTION:
//	Karp's knowledge of what lines and things do. The line specials are
//	the vanilla ones (p_switch.c P_UseSpecialLine, p_spec.c
//	P_CrossSpecialLine); keys are recognised by sprite, as P_TouchSpecialThing
//	does.
//

#include <stdlib.h>

#include "SDL.h"

#include "doomstat.h"
#include "p_local.h"
#include "r_state.h"

#include "k_specials.h"

static const char *key_names[K_NUM_KEYS] = { "blue", "yellow", "red" };

// Per sector, its range in trigger_lines.
static int *trigger_first;
static int *trigger_count;
static int *trigger_lines;

void K_Specials_Build(void)
{
    int count = SDL_max(numsectors, 1);
    int total = 0;
    int s, i;

    free(trigger_first);
    free(trigger_count);
    free(trigger_lines);
    trigger_first = calloc(count, sizeof(int));
    trigger_count = calloc(count, sizeof(int));

    // Count, then fill: few lines are movers, so the sector-by-line loops
    // stay small.
    for (s = 0; s < numsectors; ++s)
    {
        trigger_first[s] = total;

        if (sectors[s].tag == 0)
        {
            continue;
        }

        for (i = 0; i < numlines; ++i)
        {
            if (lines[i].tag == sectors[s].tag
             && K_Specials_MoverTrigger(lines[i].special) != K_TRIGGER_NONE)
            {
                ++trigger_count[s];
                ++total;
            }
        }
    }

    trigger_lines = malloc(SDL_max(total, 1) * sizeof(int));
    total = 0;

    for (s = 0; s < numsectors; ++s)
    {
        if (sectors[s].tag == 0)
        {
            continue;
        }

        for (i = 0; i < numlines; ++i)
        {
            if (lines[i].tag == sectors[s].tag
             && K_Specials_MoverTrigger(lines[i].special) != K_TRIGGER_NONE)
            {
                trigger_lines[total++] = i;
            }
        }
    }
}

k_trigger_t K_Specials_ExitTrigger(int special, int *secret)
{
    *secret = special == 51 || special == 124;

    switch (special)
    {
        case 11: case 51:
            return K_TRIGGER_USE;

        case 52: case 124:
            return K_TRIGGER_CROSS;

        default:
            return K_TRIGGER_NONE;
    }
}

k_trigger_t K_Specials_MoverTrigger(int special)
{
    switch (special)
    {
        // Switches (S1, SR). Doors that open (99 and 133-137 need a key):
        case 29: case 61: case 63: case 103: case 111: case 112: case 114:
        case 115: case 99: case 133: case 134: case 135: case 136: case 137:
        // floors:
        case 14: case 15: case 18: case 20: case 23: case 45: case 60:
        case 64: case 66: case 67: case 68: case 69: case 70: case 71:
        case 101: case 102: case 131: case 132: case 140:
        // lifts, stairs, donut:
        case 21: case 62: case 122: case 123: case 7: case 127: case 9:
            return K_TRIGGER_USE;

        // Walk-over lines (W1, WR). Doors that open:
        case 2: case 4: case 86: case 90: case 105: case 106: case 108:
        case 109:
        // floors:
        case 5: case 19: case 22: case 30: case 36: case 37: case 38:
        case 58: case 59: case 82: case 83: case 84: case 91: case 92:
        case 93: case 95: case 96: case 98: case 119: case 128: case 129:
        case 130:
        // lifts (perpetual ones too), stairs, a ceiling that rises:
        case 10: case 88: case 120: case 121: case 53: case 87: case 8:
        case 100: case 40:
            return K_TRIGGER_CROSS;

        default:
            return K_TRIGGER_NONE;
    }
}

int K_Specials_LineSpecial(int line)
{
    return lines[line].special;
}

int K_Specials_TriggerCount(int sector)
{
    return trigger_count != NULL ? trigger_count[sector] : 0;
}

int K_Specials_TriggerLine(int sector, int index)
{
    return trigger_lines[trigger_first[sector] + index];
}

int K_Specials_LockKey(int special)
{
    switch (special)
    {
        // Manual doors, and switches that open doors elsewhere
        // (EV_DoLockedDoor).
        case 26: case 32: case 99: case 133:
            return K_KEY_BLUE;

        case 27: case 34: case 136: case 137:
            return K_KEY_YELLOW;

        case 28: case 33: case 134: case 135:
            return K_KEY_RED;

        default:
            return -1;
    }
}

const char *K_Specials_KeyName(k_key_t key)
{
    return key_names[key];
}

int K_Specials_HasKey(k_key_t key)
{
    const player_t *player = &players[consoleplayer];

    switch (key)
    {
        case K_KEY_BLUE:
            return player->cards[it_bluecard] || player->cards[it_blueskull];

        case K_KEY_YELLOW:
            return player->cards[it_yellowcard] || player->cards[it_yellowskull];

        default:
            return player->cards[it_redcard] || player->cards[it_redskull];
    }
}

int K_Specials_FindKeys(k_key_t key, k_keything_t *keys, int max)
{
    static const spritenum_t cards[K_NUM_KEYS] = { SPR_BKEY, SPR_YKEY, SPR_RKEY };
    static const spritenum_t skulls[K_NUM_KEYS] = { SPR_BSKU, SPR_YSKU, SPR_RSKU };
    const thinker_t *thinker;
    int count = 0;

    for (thinker = thinkercap.next; thinker != &thinkercap; thinker = thinker->next)
    {
        const mobj_t *mo;

        if (thinker->function.acp1 != (actionf_p1) P_MobjThinker)
        {
            continue;
        }

        mo = (const mobj_t *) thinker;

        if (!(mo->flags & MF_SPECIAL)
         || (mo->sprite != cards[key] && mo->sprite != skulls[key]))
        {
            continue;
        }

        if (count < max)
        {
            keys[count].at.x = (double) mo->x / FRACUNIT;
            keys[count].at.y = (double) mo->y / FRACUNIT;
            keys[count].skull = mo->sprite == skulls[key];
        }

        ++count;
    }

    return count;
}
