//
// DESCRIPTION:
//	Karp overlay (-karpoverlay; karp/docs/plan.md, 3.12): for showing the
//	bot to an audience, the Karp logo at the top of the game's picture and
//	under it a line saying what the bot is about.
//

#ifndef __K_OVERLAY__
#define __K_OVERLAY__

struct SDL_Renderer;

// -karpoverlay. Doom's own messages stay (the user: the secret found is
// news an audience wants; under the logo they do little harm).
void K_Overlay_Enable(void);

// After I_InitGraphics: reads the logo (karp_logo.png next to the
// executable) and the font (the IWAD's HUD font).
void K_Overlay_Open(void);

// At exit, before I_ShutdownGraphics: the textures let go.
void K_Overlay_Close(void);

// Each frame, over the game's picture, before it is shown (H13).
void K_Overlay_Draw(struct SDL_Renderer *renderer);

#endif
