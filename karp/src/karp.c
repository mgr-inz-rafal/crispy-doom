//
// DESCRIPTION:
//	Karp glue: the -autoplay / -karpconsole / -quitonexit / -karpsalt /
//	-karpexit / -karpattitude / -karpdemo switches, lifecycle and the SDL
//	event filter that gives the console its own events.
//

#include <stdlib.h>
#include <time.h>

#include "SDL.h"

#include "d_event.h"
#include "d_main.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "r_state.h"

#include "karp.h"
#include "k_area.h"
#include "k_brain.h"
#include "k_combat.h"
#include "k_condition.h"
#include "k_console.h"
#include "k_demo.h"
#include "k_graph.h"
#include "k_items.h"
#include "k_log.h"
#include "k_map.h"
#include "k_nav.h"
#include "k_salt.h"
#include "k_settings.h"
#include "k_sight.h"
#include "k_specials.h"
#include "k_trace.h"

static boolean autoplay_enabled;
static boolean console_enabled;
static boolean console_open;
static boolean quit_on_exit;
static boolean level_completed;     // reported for this level
static int world_tic = -1;          // leveltime Karp last looked at the level

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

void K_EnableSalt(void)
{
    int p = M_CheckParmWithArgs("-karpsalt", 1);
    unsigned int seed;

    InitKarp();

    // A number after it is the seed; else the clock gives one.
    if (p > 0 && myargv[p + 1][0] >= '0' && myargv[p + 1][0] <= '9')
    {
        seed = (unsigned int) strtoul(myargv[p + 1], NULL, 10);
    }
    else
    {
        seed = (unsigned int) time(NULL) ^ (unsigned int) SDL_GetPerformanceCounter();
    }

    K_Salt_Enable(seed);
}

// The argument after a switch, as one of names; the first if it is none of
// them (logged).
static int Choice(const char *parm, const char **names, int count)
{
    int p = M_CheckParmWithArgs(parm, 1);
    int i;

    for (i = 0; p > 0 && i < count; ++i)
    {
        if (!strcasecmp(myargv[p + 1], names[i]))
        {
            K_Log(K_LOG_SYS, K_LOG_INFO, "%s %s", parm, names[i]);
            return i;
        }
    }

    K_Log(K_LOG_SYS, K_LOG_WARN, "%s: \"%s\" is none of its choices - %s", parm,
          p > 0 ? myargv[p + 1] : "", names[0]);
    return 0;
}

void K_EnableExitChoice(void)
{
    static const char *names[] = { "default", "random", "secret", "normal" };

    InitKarp();
    K_Brain_SetExitChoice((k_exitchoice_t) Choice("-karpexit", names, arrlen(names)));
}

void K_EnableAttitude(void)
{
    static const char *names[] = { "default", "explorer" };
    int explorer;

    InitKarp();
    explorer = Choice("-karpattitude", names, arrlen(names)) == 1;
    K_Brain_SetExplorer(explorer);
    K_Items_Explore(explorer);
}

void K_EnableDemo(void)
{
    InitKarp();
    autoplay_enabled = true;
    autostart = true;

    if (quit_on_exit)
    {
        quit_on_exit = false;
        K_Log(K_LOG_SYS, K_LOG_WARN, "-quitonexit ignored in demo mode");
    }

    K_Demo_Enable();
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

    // The specials first: the graph's jump search asks where lines send
    // sectors.
    K_Map_Build();
    K_Specials_Build();
    K_Graph_Build();
    K_Area_Build();
    K_Sight_Reset();
    K_Brain_Reset();
    K_Nav_Reset();
    K_Condition_Reset();
    K_Combat_Reset();
    K_Items_Reset();
    K_Trace_Reset();
    level_completed = false;
    world_tic = -1;

    if (K_Salt_Enabled())
    {
        K_Log(K_LOG_SYS, K_LOG_INFO, "salt: setting off after %d tics", K_Salt_StartPause());
    }

    K_Demo_LevelLoaded();
}

void K_GameLoaded(void)
{
    if (!autoplay_enabled && !console_enabled)
    {
        return;
    }

    K_Log(K_LOG_SYS, K_LOG_INFO, "savegame loaded - Karp's copy of the level built again "
          "from it");
    K_LevelLoaded();
    K_Demo_GameLoaded();
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
    boolean moved = leveltime != world_tic;

    // Only as the world moves: paused, or in a menu (P_Ticker), leveltime
    // stands still and so does Karp's picture of the level - looked at again
    // within the same tic, every item would be taken for a new one, and the
    // one gone for as picked up (the user, E2M2: paused to write a note, the
    // cell pack gone for "picked up" 3600 units off; every item listed once
    // more each tic paused).
    world_tic = leveltime;

    if (autoplay_enabled && netgame && !netgame_logged)
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "netgame - the bot stays idle, stage 1 "
              "is single player only");
        netgame_logged = true;
    }

    // A human drives: the log follows what the player does. First, so the
    // exit line crossed shows before the level ends.
    if (moved && console_enabled && !autoplay_enabled && gamestate == GS_LEVEL)
    {
        K_Trace_Tick();
    }

    // Jumps that moving sectors made possible (k_graph.h); how the player
    // is doing, what it can see, how dangerous each area is, and what the
    // items lying about are worth to it.
    if (moved && (autoplay_enabled || console_enabled) && gamestate == GS_LEVEL)
    {
        K_Graph_Tick();
        K_Condition_Tick();
        K_Combat_Tick(K_OwnsControls());
        K_Area_Tick();
        K_Items_Tick();
    }

    // An exit sets this during the tic; the intermission starts next tic.
    // First: E1M8's exit floor often kills the player in the tic it ends
    // the level.
    if ((autoplay_enabled || console_enabled || quit_on_exit)
     && gameaction == ga_completed && !level_completed)
    {
        level_completed = true;
        K_Combat_LogSummary();
        K_Brain_LevelCompleted(secretexit);

        if (quit_on_exit)
        {
            K_Log(K_LOG_SYS, K_LOG_INFO, "level finished - quitting (-quitonexit)");
            I_Quit();
        }
    }

    // On death the bot stops (the user): there is nothing more to learn
    // from the run.
    if (autoplay_enabled && quit_on_exit && gamestate == GS_LEVEL
     && K_Combat_GetStatus()->died && !level_completed)
    {
        K_Log(K_LOG_SYS, K_LOG_INFO, "the player died - quitting (-quitonexit)");
        I_Quit();
    }

    if (K_OwnsControls())
    {
        K_Brain_Tick();
    }

    K_Demo_Tick();
}

// Writes only the fields a human's keyboard and mouse would, so the command
// travels the normal way: the network, every machine's G_Ticker, demos.
void K_BuildTiccmd(ticcmd_t *cmd, int maketic)
{
    k_command_t command;
    double aim, vx, vy;
    int aiming;

    if (!K_OwnsControls())
    {
        return;
    }

    // With salt, a moment's pause before setting off (k_salt.h): hands off,
    // as a human taking in the level.
    if (leveltime < K_Salt_StartPause())
    {
        cmd->forwardmove = 0;
        cmd->sidemove = 0;
        cmd->angleturn = 0;

        if (!(cmd->buttons & BT_SPECIAL))
        {
            cmd->buttons = 0;
        }

        return;
    }

    // Nav moves the player, facing the target combat aims at where it can,
    // or, fighting, at the velocity combat wants; combat then fires if the
    // facing is on the target.
    aiming = K_Combat_Aim(maketic, &aim);

    if (K_Combat_Move(&vx, &vy))
    {
        K_Nav_BuildMoveCommand(maketic, vx, vy, aiming ? &aim : NULL, &command);
    }
    else
    {
        K_Nav_BuildCommand(maketic, aiming ? &aim : NULL, &command);
    }

    K_Combat_Fire(maketic, &command);

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
