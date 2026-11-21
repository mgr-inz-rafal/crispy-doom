//
// DESCRIPTION:
//	Karp demo mode (karp/docs/plan.md, 3.11; the user: Karp playing Doom on
//	a PC at an exhibition, 24 hours a day). With -karpdemo the bot plays on
//	by itself:
//
//	- every level's start is saved (DEMO_SLOT), on its first tic;
//	- a death, DEATH_WAIT after, loads that save again with a new random
//	  salt (k_salt.h), the run then parting from the one before - never the
//	  same death twice; so does a level still going after LEVEL_MOST (the
//	  bot stuck: never a frozen screen). With no save of this level (lost,
//	  or the bot died at once), the level starts again from scratch. Tried
//	  for ever, never skipped (the user: a level no salt clears is one Karp
//	  must learn to play);
//	- an intermission is gone through with fire, as a player would (the
//	  user): INTERMISSION_PRESS in, every stat shown at once; on after
//	  INTERMISSION_WAIT of them, in Doom 1 by the map of the episode, left
//	  INTERMISSION_PRESS later (TickIntermission);
//	- a text screen between levels (Doom II) stays as long as Doom 1 shows
//	  its own by itself - the text written out, and TEXT_WAIT more;
//	- the end of an episode - its last text, then Doom 1's end picture for
//	  END_WAIT, or Doom II's cast of characters for CAST_WAIT - starts the
//	  next one the WADs hold, after the last the first again (NextEpisode);
//	- the title screen (a game ended from the menu) starts a new game after
//	  TITLE_WAIT.
//
//	Nothing waits while the game is paused or a menu is open, and nothing
//	happens in a netgame. karp.log is rolled over once it grows past
//	LOG_MOST, keeping the one before.
//

#include <string.h>
#include <time.h>

#include "SDL.h"

#include "crispy.h"
#include "d_main.h"
#include "doomstat.h"
#include "g_game.h"
#include "m_misc.h"
#include "p_local.h"
#include "p_saveg.h"

#include "k_demo.h"
#include "k_log.h"
#include "k_salt.h"

#define DEMO_SLOT 7
#define DEATH_WAIT (3 * TICRATE)
#define LEVEL_MOST (15 * 60 * TICRATE)
#define INTERMISSION_PRESS TICRATE
#define INTERMISSION_WAIT (5 * TICRATE)
#define INTERMISSION_MOST (30 * TICRATE)
#define TEXT_WAIT 250               // f_finale.c: TEXTWAIT, after TEXTSPEED a letter
#define TEXT_SPEED 3
#define END_WAIT (30 * TICRATE)
#define CAST_WAIT (60 * TICRATE)
#define TITLE_WAIT (10 * TICRATE)
#define LOG_MOST (32L * 1024 * 1024)

extern const char *finaletext;      // f_finale.c
void F_StartCast(void);             // f_finale.c

static boolean enabled;

// The save of the level's start: whether there is one for this level, and
// where; whether the save being loaded is it.
static boolean saved;
static char save_path[512];
static boolean loading;

// The game state as last seen, and the tics since it began (paused or in a
// menu, none count); the tics the player has lain dead, and those on the
// title screen or its demos.
static int state = -1;
static int state_tics;
static int dead_tics;
static int title_tics;

// The tries at this level (1 for the first), which.
static int tries;
static int tries_episode;
static int tries_map;

// The finale: the tics its text takes to be written, as Doom 1 shows it;
// and whether Doom II's cast of characters has begun.
static int text_tics;
static boolean cast;

// The intermission: the presses of fire made, and one for the command
// being built (K_Demo_Fire).
static int presses;
static boolean fire;

static void LevelName(char *buf, int buf_len)
{
    if (gamemode == commercial)
    {
        M_snprintf(buf, buf_len, "MAP%02d", gamemap);
    }
    else
    {
        M_snprintf(buf, buf_len, "E%dM%d", gameepisode, gamemap);
    }
}

static unsigned int NewSeed(void)
{
    return (unsigned int) time(NULL) ^ (unsigned int) SDL_GetPerformanceCounter()
         ^ (unsigned int) tries * 0x9e3779b9U;
}

void K_Demo_Enable(void)
{
    enabled = true;

    K_Log(K_LOG_SYS, K_LOG_INFO, "demo mode: saving each level's start, retrying deaths with "
          "a new salt, playing on for ever");

    if (!K_Salt_Enabled())
    {
        K_Salt_Enable(NewSeed());
    }
}

int K_Demo_Enabled(void)
{
    return enabled;
}

void K_Demo_LevelLoaded(void)
{
    if (!enabled)
    {
        return;
    }

    saved = false;
    dead_tics = 0;
    K_Log_Roll(LOG_MOST);

    if (gameepisode != tries_episode || gamemap != tries_map)
    {
        tries = 1;
        tries_episode = gameepisode;
        tries_map = gamemap;
    }
}

void K_Demo_GameLoaded(void)
{
    if (!enabled)
    {
        return;
    }

    // Its own save holds the level's start; one loaded from the menu is
    // saved as the start from now on.
    saved = loading;
    loading = false;
}

// The level's start, to the save slot (on the next tic: G_SaveGame).
static void Save(void)
{
    char description[SAVESTRINGSIZE];
    char name[16];

    LevelName(name, sizeof(name));
    M_snprintf(description, sizeof(description), "KARP DEMO %s", name);
    M_StringCopy(save_path, P_SaveGameFile(DEMO_SLOT), sizeof(save_path));
    G_SaveGame(DEMO_SLOT, description);
    saved = true;

    K_Log(K_LOG_SYS, K_LOG_INFO, "demo: saving the start of %s (try %d) to %s", name, tries,
          save_path);
}

// The level again, with a new salt: from its save, or from scratch.
static void Retry(const char *why)
{
    char name[16];

    LevelName(name, sizeof(name));
    ++tries;
    K_Salt_Enable(NewSeed());

    if (saved && M_FileExists(save_path))
    {
        K_Log(K_LOG_SYS, K_LOG_INFO, "demo: %s on %s - loading its start, try %d", why, name,
              tries);
        loading = true;
        G_LoadGame(save_path);
    }
    else
    {
        K_Log(K_LOG_SYS, K_LOG_INFO, "demo: %s on %s - no save of its start, starting it "
              "again, try %d", why, name, tries);
        G_DeferedInitNew(gameskill, gameepisode, gamemap);
    }

    dead_tics = 0;
}

// The episode after this one (the user): the next whose first level the
// WADs loaded hold - Doom 1's E2 to E4 (registered, Ultimate), SIGIL's E5
// and E6; Doom II's No Rest for the Living as 2 and the Master Levels as 3,
// as crispy's menu has them - else the first again.
static int NextEpisode(void)
{
    int episode;

    if (gamemode == commercial)
    {
        return gameepisode < 2 && crispy->havenerve ? 2
             : gameepisode < 3 && crispy->havemaster ? 3 : 1;
    }

    for (episode = gameepisode + 1; episode <= 9; ++episode)
    {
        if (P_GetNumForMap(episode, 1, false) >= 0)
        {
            return episode;
        }
    }

    return 1;
}

static void NewGame(const char *why, int episode)
{
    K_Log(K_LOG_SYS, K_LOG_INFO, "demo: %s - a new game from episode %d", why, episode);
    G_DeferedInitNew(startskill, episode, 1);
}

// Whether the finale shown ends the game: every one in Doom 1 (the end of
// an episode), and in Doom II after the last level (G_WorldDone, F_Ticker).
static boolean GameEnds(void)
{
    if (gamemode != commercial)
    {
        return true;
    }

    if (gamemission == pack_nerve)
    {
        return gamemap == 8;
    }

    if (gamemission == pack_master)
    {
        return gamemap == 20 || gamemap == 21;
    }

    return gamemap == 30;
}

static void TickLevel(void)
{
    const player_t *player = &players[consoleplayer];

    if (gameaction != ga_nothing || player->mo == NULL)
    {
        return;
    }

    if (!saved && player->playerstate == PST_LIVE)
    {
        Save();
    }

    if (player->playerstate == PST_DEAD)
    {
        if (!paused && !menuactive && ++dead_tics >= DEATH_WAIT)
        {
            Retry("died");
        }
    }
    else if (leveltime >= LEVEL_MOST)
    {
        char why[64];

        M_snprintf(why, sizeof(why), "still going after %d minutes",
                   LEVEL_MOST / (60 * TICRATE));
        Retry(why);
    }
}

// Fire pressed through the intermission (WI_Ticker): the first press,
// INTERMISSION_PRESS in, shows every stat at once; the next, after
// INTERMISSION_WAIT of them, leaves them - in Doom II for the next level,
// after an episode's last level for its end (crispy's tally there held
// for it: K_HoldsTally), else in Doom 1 for the map of the episode, which
// the third press leaves INTERMISSION_PRESS later. Still there after
// INTERMISSION_MOST, on regardless.
static void TickIntermission(void)
{
    static const char *what[] = { "every stat shown", "on to the next level",
                                  "on to the map of the episode", "on to the end of the episode" };
    boolean ends = gamemode != commercial && gamemap == 8;
    int last = gamemode == commercial || ends ? 2 : 3;
    int at = INTERMISSION_PRESS + (presses > 0 ? INTERMISSION_WAIT : 0)
           + (presses > 1 ? INTERMISSION_PRESS : 0);

    if (gameaction != ga_nothing)
    {
        return;
    }

    if (state_tics == INTERMISSION_MOST)
    {
        K_Log(K_LOG_SYS, K_LOG_INFO, "demo: the intermission still on after %d s - on to the "
              "next level", INTERMISSION_MOST / TICRATE);
        G_WorldDone();
        return;
    }

    if (presses >= last || state_tics != at)
    {
        return;
    }

    K_Log(K_LOG_SYS, K_LOG_INFO, "demo: intermission - fire, %s",
          what[presses == 0 ? 0 : presses == 2 ? 1 : ends ? 3 : last == 3 ? 2 : 1]);
    ++presses;
    fire = true;
}

int K_Demo_Fire(void)
{
    boolean press = fire && gamestate == GS_INTERMISSION;

    fire = false;
    return press;
}

static void TickFinale(void)
{
    if (gameaction != ga_nothing)
    {
        return;
    }

    if (!GameEnds())
    {
        if (state_tics >= text_tics)
        {
            K_Log(K_LOG_SYS, K_LOG_INFO, "demo: the text read - on to the next level");
            gameaction = ga_worlddone;
        }
    }
    else if (gamemode == commercial)
    {
        if (state_tics >= text_tics && !cast)
        {
            cast = true;
            F_StartCast();
        }
        else if (state_tics >= text_tics + CAST_WAIT)
        {
            NewGame("the end of the episode, the cast of characters shown", NextEpisode());
        }
    }
    else if (state_tics >= text_tics + END_WAIT)
    {
        NewGame("the end of the episode", NextEpisode());
    }
}

void K_Demo_Tick(void)
{
    if (!enabled || netgame)
    {
        return;
    }

    if ((int) gamestate != state)
    {
        state = (int) gamestate;
        state_tics = 0;
        cast = false;
        presses = 0;
        fire = false;
        text_tics = (finaletext != NULL ? (int) strlen(finaletext) * TEXT_SPEED : 0) + TEXT_WAIT;
    }
    else if (!paused && !menuactive)
    {
        ++state_tics;
    }

    title_tics = demoplayback || gamestate == GS_DEMOSCREEN
               ? title_tics + (!menuactive) : 0;

    if (title_tics >= TITLE_WAIT && gameaction == ga_nothing)
    {
        title_tics = 0;
        NewGame("the title screen", 1);
        return;
    }

    switch (gamestate)
    {
        case GS_LEVEL:
            if (!demoplayback)
            {
                TickLevel();
            }
            break;

        case GS_INTERMISSION:
            TickIntermission();
            break;

        case GS_FINALE:
            TickFinale();
            break;

        default:
            break;
    }
}
