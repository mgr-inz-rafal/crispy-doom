//
// DESCRIPTION:
//	Karp - autonomous single-player bot (karp/docs/plan.md).
//
//	The only Karp header the engine includes. Every hook is a no-op
//	unless -autoplay or -karpconsole was given.
//

#ifndef __KARP__
#define __KARP__

// -autoplay: the bot drives the player.
void K_EnableAutoplay(void);

// -karpconsole: open the Karp Console window.
void K_EnableConsole(void);

#endif
