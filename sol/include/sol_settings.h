// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_settings.h — User preference storage for Sol.
 *
 * Settings are persisted in $HOME/.sol/settings.json.
 * The format is simple enough to be written and parsed without an
 * external library:
 *
 *   {
 *     "theme": {
 *       "scale": 1.0
 *     }
 *   }
 *
 * All values fall back to their documented defaults when the file is
 * absent, truncated, or contains an unrecognised key.
 */

#ifndef SOL_SETTINGS_H
#define SOL_SETTINGS_H

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define SOL_SETTINGS_UI_SCALE_MIN     0.5f
#define SOL_SETTINGS_UI_SCALE_MAX     3.0f
#define SOL_SETTINGS_UI_SCALE_DEFAULT 1.0f

/* ------------------------------------------------------------------ */
/* Settings aggregate                                                  */
/* ------------------------------------------------------------------ */

typedef struct SolSettings {
    /* ---- Theme ---- */
    /* Global UI scale factor applied via ca_instance_set_scale().
     * Range: [SOL_SETTINGS_UI_SCALE_MIN, SOL_SETTINGS_UI_SCALE_MAX].
     * Stored with two decimal places. */
    float ui_scale;
} SolSettings;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Return a SolSettings with all fields at their documented defaults. */
SolSettings sol_settings_defaults(void);

/* Load settings from $HOME/.sol/settings.json into *out.
 * On any error (file absent, parse failure, out-of-range value) the
 * affected field is replaced with its default; false is returned only
 * when the file cannot be read at all.  Callers should treat even a
 * false return as a valid *out filled with safe defaults. */
bool sol_settings_load(SolSettings *out);

/* Write *settings to $HOME/.sol/settings.json, creating the directory
 * if necessary.  Returns true on success. */
bool sol_settings_save(const SolSettings *settings);

#endif /* SOL_SETTINGS_H */
