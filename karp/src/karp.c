//
// DESCRIPTION:
//	Karp glue: the -autoplay / -karpconsole switches, lifecycle and the
//	SDL event filter that gives the console its own events.
//

#include "SDL.h"

#include "d_event.h"
#include "d_main.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "r_state.h"

#include "karp.h"
#include "k_brain.h"
#include "k_console.h"
#include "k_graph.h"
#include "k_log.h"
#include "k_map.h"
#include "k_nav.h"
#include "k_settings.h"
#include "k_specials.h"
#include "k_trace.h"

static boolean autoplay_enabled;
static boolean console_enabled;
static boolean console_open;
static boolean quit_on_exit;
static boolean level_completed;     // reported for this level

extern boolean secretexit;          // g_game.c

// Shared by both switches; safe to call more than once.
static void InitKarp(void)
{
    static boolean initialized = false;

    if (initialized)
    {
        return;
    }

    initialized = true;

    // Registered this early so it runs late at exit (the list runs newest
    // first), and the log captures everything said while shutting down.
    I_AtExit(K_Log_Shutdown, true);
}

void K_EnableAutoplay(void)
{
    InitKarp();
    autoplay_enabled = true;

    K_Log(K_LOG_SYS, K_LOG_INFO, "autoplay enabled");

    if (!M_ParmExists("-warp"))
    {
        K_Log(K_LOG_SYS, K_LOG_WARN,
              "-autoplay without -warp - the bot starts once a level is loaded");
    }
}

void K_EnableConsole(void)
{
    InitKarp();
    console_enabled = true;

    K_Log(K_LOG_SYS, K_LOG_INFO, "console enabled");
}

void K_EnableQuitOnExit(void)
{
    InitKarp();
    quit_on_exit = true;

    K_Log(K_LOG_SYS, K_LOG_INFO, "quitting when a level is finished");
}

boolean K_AutoplayEnabled(void)
{
    return autoplay_enabled;
}

// Installed only while the console exists.
static boolean EventFilter(SDL_Event *event)
{
    event_t quit;

    if (K_Console_HandleEvent(event))
    {
        return true;
    }

    // With two windows SDL sends SDL_QUIT only once the last one closes,
    // and the hidden console still counts. Without this the game's close
    // button would do nothing; with it, the usual quit prompt appears.
    if (event->type == SDL_WINDOWEVENT
     && event->window.event == SDL_WINDOWEVENT_CLOSE
     && event->window.windowID == SDL_GetWindowID(I_GetWindow()))
    {
        quit.type = ev_quit;
        D_PostEvent(&quit);
    }

    return false;
}

static void ShutdownKarp(void)
{
    if (console_open)
    {
        I_SetEventFilter(NULL);
        K_Console_Close();
        console_open = false;
    }

    K_Settings_SaveIfDirty();
}

void K_PostVideoInit(void)
{
    if (!autoplay_enabled && !console_enabled)
    {
        return;
    }

    if (console_enabled)
    {
        console_open = K_Console_Open(I_GetWindow());

        if (console_open)
        {
            I_SetEventFilter(EventFilter);
        }
    }

    // Registered after I_InitGraphics, so it runs before I_ShutdownGraphics
    // destroys every remaining SDL window.
    I_AtExit(ShutdownKarp, true);
}

void K_Frame(void)
{
    if (console_open)
    {
        K_Console_Frame();
    }
}

// With -warp this first runs inside D_DoomMain, before any window exists,
// so nothing here may depend on the console.
void K_LevelLoaded(void)
{
    if (!autoplay_enabled && !console_enabled)
    {
        return;
    }

    K_Map_Build();
    K_Graph_Build();
    K_Specials_Build();
    K_Brain_Reset();
    K_Nav_Reset();
    K_Trace_Reset();
    level_completed = false;
}

boolean K_OwnsControls(void)
{
    const player_t *player = &players[consoleplayer];

    // Stage 1 is single player only (karp/docs/plan.md, 2.4).
    return autoplay_enabled && !netgame && gamestate == GS_LEVEL
        && gameaction == ga_nothing && !paused && !menuactive && !demoplayback
        && player->mo != NULL && player->playerstate == PST_LIVE;
}

void K_Ticker(void)
{
    static boolean netgame_logged = false;

    if (autoplay_enabled && netgame && !netgame_logged)
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "netgame - the bot stays idle, stage 1 "
              "is single player only");
        netgame_logged = true;
    }

    // A human drives: the log follows what the player does. First, so the
    // exit line crossed shows before the level ends.
    if (console_enabled && !autoplay_enabled && gamestate == GS_LEVEL)
    {
        K_Trace_Tick();
    }

    // Jumps that moving sectors made possible (k_graph.h).
    if ((autoplay_enabled || console_enabled) && gamestate == GS_LEVEL)
    {
        K_Graph_Tick();
    }

    // An exit sets this during the tic; the intermission starts next tic.
    if ((autoplay_enabled || console_enabled || quit_on_exit)
     && gameaction == ga_completed && !level_completed)
    {
        level_completed = true;
        K_Brain_LevelCompleted(secretexit);

        if (quit_on_exit)
        {
            K_Log(K_LOG_SYS, K_LOG_INFO, "level finished - quitting (-quitonexit)");
            I_Quit();
        }
    }

    if (K_OwnsControls())
    {
        K_Brain_Tick();
    }
}

// Writes only the fields a human's keyboard and mouse would, so the command
// travels the normal way: the network, every machine's G_Ticker, demos.
void K_BuildTiccmd(ticcmd_t *cmd, int maketic)
{
    k_command_t command;

    if (!K_OwnsControls())
    {
        return;
    }

    K_Nav_BuildCommand(maketic, &command);

    cmd->forwardmove = command.forward;
    cmd->sidemove = command.side;
    cmd->angleturn = command.turn;
    cmd->lookfly = 0;
    cmd->arti = 0;
    cmd->lookdir = 0;

    // Pause and save still work; anything else the human pressed is dropped.
    if (!(cmd->buttons & BT_SPECIAL))
    {
        cmd->buttons = (command.use ? BT_USE : 0) | (command.attack ? BT_ATTACK : 0);

        if (command.change)
        {
            cmd->buttons |= BT_CHANGE | (command.weapon << BT_WEAPONSHIFT);
        }
    }

    // Lets the renderer interpolate the turn, as for keyboard turning.
    localview.ticangleturn = cmd->angleturn;
}
