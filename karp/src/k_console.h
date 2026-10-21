//
// DESCRIPTION:
//	Karp Console: a second SDL window running Dear ImGui
//	(karp/docs/plan.md, 3.7).
//

#ifndef __K_CONSOLE__
#define __K_CONSOLE__

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;
struct SDL_Window;

// Creates the window at its remembered placement (or a default one).
// Returns non-zero on success.
int K_Console_Open(struct SDL_Window *game_window);

// Records the final placement in the settings, then destroys the window.
void K_Console_Close(void);

// Returns non-zero if the event belongs to the console window; the game
// must not see it then.
int K_Console_HandleEvent(union SDL_Event *event);

// Draws one console frame, at most ~60 times a second.
void K_Console_Frame(void);

#ifdef __cplusplus
}
#endif

#endif
