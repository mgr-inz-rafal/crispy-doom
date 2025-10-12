#pragma once

#include <SDL.h>

extern int resolver_opened;

void EnableResolver();
void DisableResolver();
void ResolverMouseEvent(SDL_Event *sdlevent);