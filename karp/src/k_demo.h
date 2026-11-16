//
// DESCRIPTION:
//	Karp demo mode (karp/docs/plan.md, 3.11): -karpdemo plays on by itself,
//	level after level, for as long as the game runs - an exhibition PC
//	showing Doom all day. Each level's start is saved; a death loads it
//	again with another salt; intermissions and text screens move on by
//	themselves; the end of the game starts it again from the first level.
//
//	Plain C types only.
//

#ifndef __K_DEMO__
#define __K_DEMO__

#ifdef __cplusplus
extern "C" {
#endif

// Turns demo mode on; logged.
void K_Demo_Enable(void);

int K_Demo_Enabled(void);

// End of P_SetupLevel, after Karp has built its copy of the level.
void K_Demo_LevelLoaded(void);

// After a savegame has been loaded, Karp's copy rebuilt from it.
void K_Demo_GameLoaded(void);

// Once a game tic, after G_Ticker and the bot's own tic.
void K_Demo_Tick(void);

#ifdef __cplusplus
}
#endif

#endif
