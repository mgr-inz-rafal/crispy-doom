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

#ifdef __cplusplus
}
#endif

#endif
