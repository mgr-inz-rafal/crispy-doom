//
// DESCRIPTION:
//	Karp salt (karp/docs/plan.md, follow-up 17): -karpsalt [seed] makes
//	runs differ, so a run is not taken for a demo played back.
//
//	Plain C types only.
//

#ifndef __K_SALT__
#define __K_SALT__

#ifdef __cplusplus
extern "C" {
#endif

// Turns salt on with the seed; logged.
void K_Salt_Enable(unsigned int seed);

int K_Salt_Enabled(void);

// x mixed with the seed, for the bot's own hashes; x itself without salt.
unsigned int K_Salt_Mix(unsigned int x);

// A number in -1..1 drawn from x and the seed; 0 without salt.
double K_Salt_Unit(unsigned int x);

// Tics the bot waits before it sets off on this level: up to half a
// second with salt, none without.
int K_Salt_StartPause(void);

#ifdef __cplusplus
}
#endif

#endif
