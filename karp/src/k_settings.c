//
// DESCRIPTION:
//	Karp settings file: plain text, one "key value" per line. Lines
//	starting with '#' are comments. Unknown keys are kept and written
//	back, so newer and older builds can share the file.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m_config.h"
#include "m_misc.h"

#include "k_log.h"
#include "k_settings.h"

#define MAX_SETTINGS 64
#define MAX_KEY_LEN 48
#define MAX_VALUE_LEN 80

typedef struct
{
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
} setting_t;

static setting_t settings[MAX_SETTINGS];
static int num_settings;
static boolean loaded;
static boolean dirty;

static char *SettingsPath(void)
{
    static char *path = NULL;

    if (path == NULL)
    {
        path = M_StringJoin(configdir != NULL ? configdir : "", "karp.cfg",
                            NULL);
    }

    return path;
}

static boolean LoadFile(const char *path)
{
    char line[256];
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
    FILE *file;

    file = M_fopen(path, "r");

    if (file == NULL)
    {
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL
        && num_settings < MAX_SETTINGS)
    {
        // The value is the rest of the line after the key.
        if (line[0] != '#'
         && sscanf(line, "%47s %79[^\r\n]", key, value) == 2)
        {
            M_StringCopy(settings[num_settings].key, key, MAX_KEY_LEN);
            M_StringCopy(settings[num_settings].value, value, MAX_VALUE_LEN);
            ++num_settings;
        }
    }

    fclose(file);
    return true;
}

static void LoadSettings(void)
{
    char *temp_path;

    if (loaded)
    {
        return;
    }

    loaded = true;

    if (!LoadFile(SettingsPath()))
    {
        // A crash between removing karp.cfg and renaming the new file into
        // place leaves only the temporary file.
        temp_path = M_StringJoin(SettingsPath(), ".tmp", NULL);
        LoadFile(temp_path);
        free(temp_path);
    }
}

static setting_t *FindSetting(const char *key)
{
    int i;

    for (i = 0; i < num_settings; ++i)
    {
        if (!strcmp(settings[i].key, key))
        {
            return &settings[i];
        }
    }

    return NULL;
}

int K_Settings_GetInt(const char *key, int default_value)
{
    setting_t *setting;

    LoadSettings();
    setting = FindSetting(key);

    return setting != NULL ? atoi(setting->value) : default_value;
}

int K_Settings_SetInt(const char *key, int value)
{
    char text[MAX_VALUE_LEN];
    setting_t *setting;

    LoadSettings();
    M_snprintf(text, sizeof(text), "%d", value);
    setting = FindSetting(key);

    if (setting == NULL)
    {
        if (num_settings == MAX_SETTINGS)
        {
            K_Log(K_LOG_SYS, K_LOG_WARN, "karp.cfg is full - %s not saved",
                  key);
            return 0;
        }

        setting = &settings[num_settings++];
        M_StringCopy(setting->key, key, MAX_KEY_LEN);
        setting->value[0] = '\0';
    }

    if (!strcmp(setting->value, text))
    {
        return 0;
    }

    M_StringCopy(setting->value, text, MAX_VALUE_LEN);
    dirty = true;
    return 1;
}

void K_Settings_SaveIfDirty(void)
{
    char *temp_path;
    FILE *file;
    boolean ok;
    int i;

    if (!dirty)
    {
        return;
    }

    // Write a temporary file first, so a crash mid-write cannot leave a
    // truncated karp.cfg behind.
    temp_path = M_StringJoin(SettingsPath(), ".tmp", NULL);
    file = M_fopen(temp_path, "w");

    if (file == NULL)
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "could not write %s", temp_path);
        free(temp_path);
        return;
    }

    fprintf(file, "# Karp settings - written by Karp, safe to delete\n");

    for (i = 0; i < num_settings; ++i)
    {
        fprintf(file, "%-20s %s\n", settings[i].key, settings[i].value);
    }

    ok = fclose(file) == 0;

    // rename() does not replace an existing file on Windows.
    if (ok)
    {
        M_remove(SettingsPath());
        ok = M_rename(temp_path, SettingsPath()) == 0;
    }

    if (ok)
    {
        dirty = false;
    }
    else
    {
        K_Log(K_LOG_SYS, K_LOG_WARN, "could not save %s", SettingsPath());
    }

    free(temp_path);
}
