//
// DESCRIPTION:
//	Karp glue: the -autoplay / -karpconsole switches and lifecycle.
//

#include "i_system.h"
#include "m_argv.h"

#include "karp.h"
#include "k_log.h"

static boolean autoplay_enabled;
static boolean console_enabled;

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
