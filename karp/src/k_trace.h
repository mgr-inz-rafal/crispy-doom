//
// DESCRIPTION:
//	Karp player trace: while a human drives (-karpconsole without
//	-autoplay), what the player does goes to the log - where it goes,
//	what it uses, crosses and shoots, what moves, what it picks up and
//	what it destroys - so a level played through by hand shows the way
//	the bot should find (karp/docs/plan.md, 3.10).
//

#ifndef __K_TRACE__
#define __K_TRACE__

#ifdef __cplusplus
extern "C" {
#endif

// On every level load.
void K_Trace_Reset(void);

// Once a tic, after G_Ticker, while a human drives in a level.
void K_Trace_Tick(void);

#ifdef __cplusplus
}
#endif

#endif
