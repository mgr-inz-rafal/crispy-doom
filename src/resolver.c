#include "resolver.h"

#include "SDL.h"

#include <stdio.h>

static SDL_Window *debug_window;
static SDL_Renderer *debug_renderer;

int resolver_opened = 0;

void OpenDebugWindow()
{
    debug_window =
        SDL_CreateWindow("Resolver", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, 1920, 1080, SDL_WINDOW_SHOWN);

    SDL_SetWindowPosition(debug_window, 3000, 0);

    debug_renderer =
        SDL_CreateRenderer(debug_window, -1, SDL_RENDERER_ACCELERATED);

    // Render to first window
    SDL_SetRenderDrawColor(debug_renderer, 0, 0, 0, 255); // Red
    SDL_RenderClear(debug_renderer);

    SDL_SetRenderDrawColor(debug_renderer, 255, 0, 0, 255);
    SDL_RenderDrawLine(debug_renderer, 10, 10, 200, 200);

    SDL_RenderPresent(debug_renderer);
}


void EnableResolver()
{
    if (resolver_opened == 1)
        return;
    printf("[RESOLVER] Opening debug window\n");
    resolver_opened = 1;
    OpenDebugWindow();
}

void DisableResolver()
{
    if (resolver_opened == 0)
        return;
    printf("[RESOLVER] Closing debug window\n");
    resolver_opened = 0;
    SDL_DestroyRenderer(debug_renderer);
    SDL_DestroyWindow(debug_window);
}

void ResolverMouseEvent(SDL_Event *sdlevent)
{
    int x = sdlevent->button.x;
    int y = sdlevent->button.y;

    printf("[RESOLVER] Mouse event: type=%d\n", sdlevent->type);

    printf("Mouse down at %d,%d\n", x, y);

    // Draw a single pixel at that position
    SDL_SetRenderDrawColor(debug_renderer, 0, 255, 0, 255); // red pixel
    SDL_RenderDrawPoint(debug_renderer, x, y);
    SDL_RenderPresent(debug_renderer);
}