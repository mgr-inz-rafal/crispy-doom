//
// DESCRIPTION:
//	Karp rolling log: the bot's "thought stream" - what it decided and
//	why, not a per-tic dump (karp/docs/plan.md, 3.8).
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_LOG__
#define __K_LOG__

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define K_PRINTF_ATTR(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define K_PRINTF_ATTR(fmt, first)
#endif

#define K_LOG_LINE_LEN 200

typedef enum
{
    K_LOG_SYS,          // startup, windows
    K_LOG_MAP,          // level load stats, sanity checks
    K_LOG_BRAIN,        // priority changes and the reasons for them
    K_LOG_NAV,          // paths, replans, stuck, arrival
    K_LOG_CTRL,         // controller details, disabled by default
    K_LOG_NUMCATEGORIES
} k_logcat_t;

typedef enum
{
    K_LOG_INFO,
    K_LOG_WARN,
    K_LOG_ERROR
} k_loglevel_t;

typedef struct
{
    int leveltime;      // tics into the level when first logged
    k_logcat_t category;
    k_loglevel_t level;
    int repeat;         // identical consecutive lines collapse into one
    char text[K_LOG_LINE_LEN];
} k_logentry_t;

// Log state changes only, never every tic. Writes to the ring buffer,
// stdout and karp.log.
void K_Log(k_logcat_t category, k_loglevel_t level, const char *fmt, ...)
    K_PRINTF_ATTR(3, 4);

// Ring buffer access for the console. Index 0 is the oldest entry.
int K_Log_Count(void);
const k_logentry_t *K_Log_Entry(int index);

// Changes whenever an entry is added or updated, so readers can tell
// whether anything is new without rescanning.
unsigned int K_Log_Serial(void);

// Empties the ring buffer only; stdout and karp.log are unaffected.
void K_Log_Clear(void);

// Messages in disabled categories are dropped entirely.
void K_Log_SetCategoryEnabled(k_logcat_t category, int enable);
int K_Log_CategoryEnabled(k_logcat_t category);

const char *K_Log_CategoryName(k_logcat_t category);

// Writes a pending repeat count and closes karp.log. Run via I_AtExit.
void K_Log_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
