//
// DESCRIPTION:
//	Karp salt (karp/docs/plan.md, follow-up 17). The bot is deterministic:
//	the same build on the same map plays the same game to the tic, and a
//	viewer may take a run for a demo played back. With -karpsalt, small
//	choices vary within what they allow anyway: a moment's pause before
//	setting off on a level, how long the player strafes one way in a fight,
//	which way it steps aside when stuck, where on the target it aims. The
//	first command that differs changes how every monster moves, so runs
//	part from there. The numbers are Karp's own, never P_Random (plan,
//	2.4), so demos still record the commands and play back exactly; the
//	seed is logged, so a run can be repeated.
//

#include "doomdef.h"
#include "doomstat.h"

#include "k_log.h"
#include "k_salt.h"

// The longest pause before setting off on a level.
#define START_PAUSE_MOST (TICRATE / 2)

static int enabled;
static unsigned int seed;

static unsigned int Hash(unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void K_Salt_Enable(unsigned int value)
{
    enabled = 1;
    seed = value;
    K_Log(K_LOG_SYS, K_LOG_INFO, "salt enabled, seed %u (-karpsalt %u repeats this run)",
          seed, seed);
}

int K_Salt_Enabled(void)
{
    return enabled;
}

unsigned int K_Salt_Mix(unsigned int x)
{
    return enabled ? x ^ Hash(seed * 0x9e3779b9U + 0x632be5abU) : x;
}

double K_Salt_Unit(unsigned int x)
{
    if (!enabled)
    {
        return 0.0;
    }

    return (Hash(K_Salt_Mix(x)) & 0xffff) / 32767.5 - 1.0;
}

int K_Salt_StartPause(void)
{
    if (!enabled)
    {
        return 0;
    }

    return Hash(K_Salt_Mix((unsigned int) (gameepisode * 100 + gamemap))) % (START_PAUSE_MOST + 1);
}
