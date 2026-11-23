//
// DESCRIPTION:
//	Karp rolling log: ring buffer for the console, mirrored to stdout
//	and to karp.log in crispy's config directory (the exe folder on
//	Windows), begun anew on every run - the one before moved to
//	ARCHIVE_DIR, never overwritten.
//

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "doomstat.h"
#include "doomtype.h"
#include "i_timer.h"
#include "m_config.h"
#include "m_misc.h"

#include "k_log.h"

#define K_LOG_MAX_ENTRIES 2048

// The logs of earlier runs (the user: "add some timestamp to the logs, so we
// never overwrite"), and the parts a long demo run rolled over (K_Log_Roll):
// moved to this folder beside karp.log, each named by when it was last
// written - karp-20261123-184210.log, -2 and on if taken.
#define ARCHIVE_DIR "karp-logs"
#define ARCHIVE_TRIES 100

static k_logentry_t entries[K_LOG_MAX_ENTRIES];
static int first;               // ring index of the oldest entry
static int count;
static unsigned int serial;

static boolean enabled[K_LOG_NUMCATEGORIES] =
{
    true,       // K_LOG_SYS
    true,       // K_LOG_MAP
    true,       // K_LOG_BRAIN
    true,       // K_LOG_NAV
    false,      // K_LOG_CTRL
    true,       // K_LOG_PLAYER
    true,       // K_LOG_COMBAT
};

static const char *category_names[K_LOG_NUMCATEGORIES] =
{
    "SYS", "MAP", "BRAIN", "NAV", "CTRL", "PLAYER", "COMBAT",
};

static const char *level_prefixes[] =
{
    "",             // K_LOG_INFO
    "WARNING: ",    // K_LOG_WARN
    "ERROR: ",      // K_LOG_ERROR
};

static FILE *logfile;
static boolean logfile_failed;
static boolean shut_down;

static k_logentry_t *LastEntry(void)
{
    if (count == 0)
    {
        return NULL;
    }

    return &entries[(first + count - 1) % K_LOG_MAX_ENTRIES];
}

// configdir is set long before Karp logs anything; fall back to the
// working directory just in case.
static char *LogPath(const char *name)
{
    return M_StringJoin(configdir != NULL ? configdir : "", name, NULL);
}

// karp.log, if there is one, moved to ARCHIVE_DIR under the time it was
// last written.
static void ArchiveLog(void)
{
    char *path = LogPath("karp.log");
    char *dir = LogPath(ARCHIVE_DIR);
    char name[64], stamp[32];
    struct stat info;
    struct tm *when;
    time_t written;
    int n;

    if (M_stat(path, &info) == 0)
    {
        written = info.st_mtime;
        when = localtime(&written);

        if (when == NULL || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", when) == 0)
        {
            M_StringCopy(stamp, "unknown", sizeof(stamp));
        }

        M_MakeDirectory(dir);

        for (n = 1; n <= ARCHIVE_TRIES; ++n)
        {
            char *target;

            if (n == 1)
            {
                M_snprintf(name, sizeof(name), "karp-%s.log", stamp);
            }
            else
            {
                M_snprintf(name, sizeof(name), "karp-%s-%d.log", stamp, n);
            }

            target = M_StringJoin(dir, DIR_SEPARATOR_S, name, NULL);

            if (!M_FileExists(target))
            {
                if (M_rename(path, target) != 0)
                {
                    printf("Karp: could not move %s to %s\n", path, target);
                }

                free(target);
                break;
            }

            free(target);
        }
    }

    free(path);
    free(dir);
}

static void OpenLogFile(void)
{
    char *path;

    // Never reopen after shutdown: "w" would truncate the finished log.
    if (logfile != NULL || logfile_failed || shut_down)
    {
        return;
    }

    ArchiveLog();
    path = LogPath("karp.log");
    logfile = M_fopen(path, "w");

    if (logfile == NULL)
    {
        logfile_failed = true;
        printf("Karp: could not open %s for writing\n", path);
    }

    free(path);
}

static void WriteToSinks(const k_logentry_t *entry, const char *text)
{
    char line[K_LOG_LINE_LEN + 64];
    char time[16];

    K_Log_FormatTime(entry->leveltime, time, sizeof(time));
    M_snprintf(line, sizeof(line), "%s %-6s %s%s", time,
               category_names[entry->category],
               level_prefixes[entry->level], text);

    printf("Karp %s\n", line);

    OpenLogFile();

    if (logfile != NULL)
    {
        fprintf(logfile, "%s\n", line);
        fflush(logfile);
    }
}

// The first occurrence of a line is written straight away; the total
// count of a collapsed run is written once the run ends.
static void FlushRepeat(void)
{
    const k_logentry_t *last = LastEntry();
    char text[K_LOG_LINE_LEN + 16];

    if (last != NULL && last->repeat > 1)
    {
        M_snprintf(text, sizeof(text), "%s (x%d)", last->text, last->repeat);
        WriteToSinks(last, text);
    }
}

void K_Log(k_logcat_t category, k_loglevel_t level, const char *fmt, ...)
{
    char text[K_LOG_LINE_LEN];
    k_logentry_t *entry;
    va_list args;

    if (!enabled[category])
    {
        return;
    }

    va_start(args, fmt);
    M_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    entry = LastEntry();

    if (entry != NULL && entry->category == category && entry->level == level
     && !strcmp(entry->text, text))
    {
        ++entry->repeat;
        ++serial;
        return;
    }

    FlushRepeat();

    if (count < K_LOG_MAX_ENTRIES)
    {
        ++count;
    }
    else
    {
        first = (first + 1) % K_LOG_MAX_ENTRIES;
    }

    entry = LastEntry();
    entry->leveltime = leveltime;
    entry->category = category;
    entry->level = level;
    entry->repeat = 1;
    M_StringCopy(entry->text, text, sizeof(entry->text));
    ++serial;

    WriteToSinks(entry, entry->text);
}

int K_Log_Count(void)
{
    return count;
}

const k_logentry_t *K_Log_Entry(int index)
{
    if (index < 0 || index >= count)
    {
        return NULL;
    }

    return &entries[(first + index) % K_LOG_MAX_ENTRIES];
}

unsigned int K_Log_Serial(void)
{
    return serial;
}

void K_Log_Clear(void)
{
    FlushRepeat();
    first = 0;
    count = 0;
    ++serial;
}

void K_Log_SetCategoryEnabled(k_logcat_t category, int enable)
{
    enabled[category] = enable != 0;
}

int K_Log_CategoryEnabled(k_logcat_t category)
{
    return enabled[category];
}

const char *K_Log_CategoryName(k_logcat_t category)
{
    return category_names[category];
}

void K_Log_FormatTime(int tics, char *buf, int buf_len)
{
    M_snprintf(buf, buf_len, "%02d:%02d.%d", tics / (60 * TICRATE),
               (tics / TICRATE) % 60, (tics % TICRATE) * 10 / TICRATE);
}

void K_Log_Roll(long most)
{
    if (logfile == NULL || ftell(logfile) < most)
    {
        return;
    }

    // Opened again, the full one is moved to the archive first.
    fclose(logfile);
    logfile = NULL;
    OpenLogFile();
}

void K_Log_Shutdown(void)
{
    FlushRepeat();

    if (logfile != NULL)
    {
        fclose(logfile);
        logfile = NULL;
    }

    shut_down = true;
}
