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

// -autoplay: the bot drives the player.
void K_EnableAutoplay(void);

// -karpconsole: open the Karp Console window.
void K_EnableConsole(void);

// Right after I_InitGraphics: opens the console.
void K_PostVideoInit(void);

// Once per rendered frame: redraws the console (rate-limited).
void K_Frame(void);

// End of P_SetupLevel: rebuilds Karp's copy of the level.
void K_LevelLoaded(void);

boolean K_AutoplayEnabled(void);

#endif
