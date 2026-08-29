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
 *       "scale": 1.0,
 *       "style": "com.sol.theme.glass",
 *       "effect": "com.sol.shaders.aurora",
 *       "opacity": 1.0
 *     },
 *     "appearance": {
 *       "corner_radius": 12.0,
 *       "panel_blur": 16.0,
 *       "titlebar_blur": 12.0,
 *       "panel_opacity": 1.0,
 *       "scrollbar_width": 8.0
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

#define SOL_SETTINGS_UI_SCALE_MIN       0.5f
#define SOL_SETTINGS_UI_SCALE_MAX       3.0f
#define SOL_SETTINGS_UI_SCALE_DEFAULT   1.0f

#define SOL_SETTINGS_BG_OPACITY_MIN     0.0f
#define SOL_SETTINGS_BG_OPACITY_MAX     1.0f
#define SOL_SETTINGS_BG_OPACITY_DEFAULT 1.0f
#define SOL_SETTINGS_BG_EFFECT_ID_MAX   63
#define SOL_SETTINGS_THEME_ID_MAX       63
#define SOL_SETTINGS_THEME_ID_DEFAULT   "com.sol.theme.midnight"
#define SOL_SETTINGS_THEME_ID_FALLBACK  "com.sol.theme.glass"
#define SOL_SETTINGS_BG_EFFECT_ID_DEFAULT "com.sol.bfx.aurora"

/* Appearance overlay tunables */
#define SOL_SETTINGS_CORNER_RADIUS_MIN     0.0f
#define SOL_SETTINGS_CORNER_RADIUS_MAX     20.0f
#define SOL_SETTINGS_CORNER_RADIUS_DEFAULT 8.0f

#define SOL_SETTINGS_PANEL_BLUR_MIN     0.0f
#define SOL_SETTINGS_PANEL_BLUR_MAX     40.0f
#define SOL_SETTINGS_PANEL_BLUR_DEFAULT 16.0f

#define SOL_SETTINGS_TITLEBAR_BLUR_MIN     0.0f
#define SOL_SETTINGS_TITLEBAR_BLUR_MAX     40.0f
#define SOL_SETTINGS_TITLEBAR_BLUR_DEFAULT 12.0f

/* Background-effect blur passes for chrome regions (titlebar/statusbar/sidebar).
 * Integer 0–4; stored as float for slider compatibility. */
#define SOL_SETTINGS_BG_BLUR_MIN     0.0f
#define SOL_SETTINGS_BG_BLUR_MAX     4.0f
#define SOL_SETTINGS_BG_BLUR_DEFAULT 3.0f

/* Background-effect blur passes for the centre buffer area.  The editor stays
 * deliberately sharper than chrome so text remains crisp. */
#define SOL_SETTINGS_BUFFER_BLUR_MIN     0.0f
#define SOL_SETTINGS_BUFFER_BLUR_MAX     2.0f
#define SOL_SETTINGS_BUFFER_BLUR_DEFAULT 1.0f

#define SOL_SETTINGS_PANEL_OPACITY_MIN     0.0f
#define SOL_SETTINGS_PANEL_OPACITY_MAX     1.0f
#define SOL_SETTINGS_PANEL_OPACITY_DEFAULT 1.0f

#define SOL_SETTINGS_SCROLLBAR_WIDTH_MIN     2.0f
#define SOL_SETTINGS_SCROLLBAR_WIDTH_MAX     20.0f
#define SOL_SETTINGS_SCROLLBAR_WIDTH_DEFAULT 8.0f

/* ------------------------------------------------------------------ */
/* Settings aggregate                                                  */
/* ------------------------------------------------------------------ */

/*
 * Aggregate of all user-configurable preferences persisted in settings.json.
 *
 * Fields are safe to read at any time; write via sol_settings_save.
 */
typedef struct SolSettings {
    /* ---- Theme ---- */
    /* Global UI scale factor applied via ca_instance_set_scale().
     * Range: [SOL_SETTINGS_UI_SCALE_MIN, SOL_SETTINGS_UI_SCALE_MAX].
     * Stored with two decimal places. */
    float ui_scale;

    /* Active complete CSS theme id. */
    char theme_id[SOL_SETTINGS_THEME_ID_MAX + 1];

    /* Active background shader effect id (dotted, e.g. "com.sol.shaders.aurora").
     * Empty string means no effect is active. */
    char bg_effect_id[SOL_SETTINGS_BG_EFFECT_ID_MAX + 1];

    /* Global background effect opacity [SOL_SETTINGS_BG_OPACITY_MIN, SOL_SETTINGS_BG_OPACITY_MAX].
     * Applied as the alpha/intensity of the rendered effect. */
    float bg_opacity;

    /* Background-effect blur passes for chrome (titlebar/statusbar/sidebar).
     * Range: [SOL_SETTINGS_BG_BLUR_MIN, SOL_SETTINGS_BG_BLUR_MAX]. */
    float bg_blur;

    /* Background-effect blur passes for the centre buffer area.
     * Range: [SOL_SETTINGS_BUFFER_BLUR_MIN, SOL_SETTINGS_BUFFER_BLUR_MAX]. */
    float buffer_blur;

    /* ---- Appearance overlay ---- */
    /* Corner radius (px) applied to panels, cards, buttons, inputs, etc.
     * Range: [SOL_SETTINGS_CORNER_RADIUS_MIN, SOL_SETTINGS_CORNER_RADIUS_MAX]. */
    float corner_radius;

    /* Backdrop blur for buffer + side panels (px).
     * Range: [SOL_SETTINGS_PANEL_BLUR_MIN, SOL_SETTINGS_PANEL_BLUR_MAX]. */
    float panel_blur;

    /* Backdrop blur for the titlebar (px).
     * Range: [SOL_SETTINGS_TITLEBAR_BLUR_MIN, SOL_SETTINGS_TITLEBAR_BLUR_MAX]. */
    float titlebar_blur;

    /* Overall UI panel opacity multiplier.  Applied by scaling all panel
     * background alpha values uniformly via a CSS override.
     * Range: [SOL_SETTINGS_PANEL_OPACITY_MIN, SOL_SETTINGS_PANEL_OPACITY_MAX]. */
    float panel_opacity;

    /* Scrollbar thumb width (px). The radius is always half this value so
     * every scrollbar remains pill-shaped at every configured width.
     * Range: [SOL_SETTINGS_SCROLLBAR_WIDTH_MIN, SOL_SETTINGS_SCROLLBAR_WIDTH_MAX]. */
    float scrollbar_width;
} SolSettings;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Return a SolSettings with all fields initialised to their documented defaults. */
SolSettings sol_settings_defaults(void);

/*
 * Load settings from $HOME/.sol/settings.json.
 *
 * On any error (file absent, parse failure, out-of-range value) the affected
 * field is replaced with its default. false is returned only when the file
 * cannot be read at all; callers should treat even a false return as a valid
 * *out filled with safe defaults.
 *
 * out  Receives the loaded (or defaulted) settings.
 * Returns  true if the file was read successfully.
 */
bool sol_settings_load(SolSettings *out);

/*
 * Write settings to $HOME/.sol/settings.json, creating the directory if needed.
 *
 * settings  Settings to persist.
 * Returns   true on success.
 */
bool sol_settings_save(const SolSettings *settings);

/*
 * Build a CSS override snippet that encodes the appearance overlay fields.
 *
 * Writes a NUL-terminated CSS string into buf (up to bufsz bytes).
 * The resulting CSS is meant to be appended after the active theme CSS so it
 * takes precedence via specificity / declaration order.
 *
 * settings  Source of appearance values.
 * buf       Destination buffer.
 * bufsz     Size of buf in bytes.
 * Returns   Number of bytes written (excluding NUL), or 0 on failure.
 */
int sol_settings_build_appearance_css(const SolSettings *settings,
                                      char *buf, int bufsz);

#endif /* SOL_SETTINGS_H */
