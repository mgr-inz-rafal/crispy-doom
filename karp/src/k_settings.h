//
// DESCRIPTION:
//	Karp's own persistent settings, kept in karp.cfg next to crispy's
//	config (karp/docs/plan.md, 3.9). crispy-doom.cfg is never touched.
//
//	Plain C types only: the console (C++) includes this header.
//

#ifndef __K_SETTINGS__
#define __K_SETTINGS__

#ifdef __cplusplus
extern "C" {
#endif

// karp.cfg is loaded on first use.
int K_Settings_GetInt(const char *key, int default_value);

// Returns non-zero if the stored value changed (which marks the file dirty).
int K_Settings_SetInt(const char *key, int value);

// Writes karp.cfg via a temporary file, if anything changed since the last
// write.
void K_Settings_SaveIfDirty(void);

#ifdef __cplusplus
}
#endif

#endif
