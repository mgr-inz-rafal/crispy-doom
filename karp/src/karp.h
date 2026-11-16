//
// DESCRIPTION:
//	Karp - autonomous single-player bot (karp/docs/plan.md).
//
//	The only Karp header the engine includes. Every hook is a no-op
//	unless -autoplay or -karpconsole was given.
//

#ifndef __KARP__
#define __KARP__

#include "doomtype.h"
#include "d_ticcmd.h"

// -autoplay: the bot drives the player.
void K_EnableAutoplay(void);

// -karpconsole: open the Karp Console window.
void K_EnableConsole(void);

// -quitonexit: quit as soon as a level is finished (for test runs).
void K_EnableQuitOnExit(void);

// -karpsalt [seed]: runs differ, from the seed given or the clock.
void K_EnableSalt(void);

// -karpdemo: the bot plays on by itself, episode after episode (k_demo.h); implies
// -autoplay and salt, starts a game at once, overrides -quitonexit.
void K_EnableDemo(void);

// Right after I_InitGraphics: opens the console.
void K_PostVideoInit(void);

// Once per rendered frame: redraws the console (rate-limited).
void K_Frame(void);

// End of P_SetupLevel: rebuilds Karp's copy of the level.
void K_LevelLoaded(void);

// End of G_DoLoadGame: rebuilds it again from the world the savegame
// restored (P_SetupLevel ran before, on the level as it starts; every
// thing and mover has been replaced since).
void K_GameLoaded(void);

// Once per game tic, after G_Ticker: the bot thinks on the fresh state.
void K_Ticker(void);

// End of G_BuildTiccmd: while the bot owns the controls, writes its input
// into the command built for the tic maketic.
void K_BuildTiccmd(ticcmd_t *cmd, int maketic);

// Whether the bot drives the player right now: -autoplay, in a level, no
// menu or pause, player alive. Human movement and mouse look are ignored
// while it does.
boolean K_OwnsControls(void);

boolean K_AutoplayEnabled(void);

#endif
