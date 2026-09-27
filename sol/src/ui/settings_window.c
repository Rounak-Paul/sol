// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* settings_window.c — Sol Settings window.
 *
 * Two-panel layout (760 × 540): a tab list on the left, the active tab's
 * page on the right.
 *
 *   Theme        Theme / style / background dropdowns with hover preview
 *                (revert-on-dismiss via preview_*_id snapshots), UI scale,
 *                background intensity, and appearance sliders.
 *   Preferences  Behavioural preferences: autosave and its delay, caret
 *                blink, hidden files in the explorer and file pickers.
 *                Persisted to settings.json and pushed into the live UI
 *                through sol_ui_system_apply_preferences.
 *   Keybindings  Leader modifier plus one row per action with an editable
 *                chord. Changes are validated against the live registry
 *                (no duplicate or prefix-overlapping chords), written to
 *                bindings.conf surgically, and applied by publishing
 *                SOL_EVENT_BINDINGS_CHANGED so the host reloads the keymap.
 *
 * Every page re-reads its source of truth on render and subscribes to the
 * signals that change it (theme / effect / prefs / flow-registry revs), so
 * edits made elsewhere — a menu toggle, another Sol instance — show up live.
 *
 * At most one settings window is open per UI system; re-opening switches
 * the existing window to the requested tab.
 */

#include "sol_ui_internal.h"
#include "sol_bg_effect.h"
#include "sol_config.h"
#include "sol_event.h"
#include "sol_settings.h"
#include "sol_ui_system.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define SW_DEFAULT_WIDTH   760
#define SW_DEFAULT_HEIGHT  540

#define SW_TAB_COUNT  ((int)SOL_UI_SETTINGS_TAB_COUNT)

static const char * const SW_TAB_LABELS[SW_TAB_COUNT] = {
    [SOL_UI_SETTINGS_TAB_THEME]       = "Theme",
    [SOL_UI_SETTINGS_TAB_PREFERENCES] = "Preferences",
    [SOL_UI_SETTINGS_TAB_KEYBINDINGS] = "Keybindings",
};

#define SW_MAX_DEFAULT_BINDINGS 96
#define SW_MAX_BIND_ROWS   (SOL_UI_MAX_COMMAND_FLOWS + SW_MAX_DEFAULT_BINDINGS)
#define SW_CHORD_MAX       64
#define SW_STATUS_MAX      192

#define SW_LEADER_COUNT 4
static const char * const SW_LEADER_NAMES[SW_LEADER_COUNT] = { "Ctrl", "Alt", "Super", "Shift" };
static const SolModifierMask SW_LEADER_MODS[SW_LEADER_COUNT] = {
    SOL_MOD_CTRL, SOL_MOD_ALT, SOL_MOD_SUPER, SOL_MOD_SHIFT,
};

#define SW_MAX_EFFECTS  33   /* 1 "None" + up to SOL_BG_EFFECT_MAX */
#define SW_MAX_THEMES   SOL_THEME_MAX
#define SW_MAX_STYLES   SOL_THEME_MAX  /* styles reuse the theme registry */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolSettingsWindow SolSettingsWindow;

typedef struct {
    SolSettingsWindow *win;
    int                tab_index;
} SwTabCtx;

/* One editable action row on the Keybindings tab. */
typedef struct {
    SolSettingsWindow *win;
    char   action[SOL_UI_MAX_ACTION_LEN + 1u];
    char   committed[SW_CHORD_MAX];   /* chord last synced from the registry */
    char   draft[SW_CHORD_MAX];       /* text currently in the input        */
    char   fallback[SW_CHORD_MAX];    /* default chord, "" when none        */
    bool   bound;                     /* action currently has a chord       */
    bool   seen;                      /* scratch flag for row rebuilds      */
} SwBindRow;

struct SolSettingsWindow {
    Ca_Window           *window;
    Ca_Instance         *instance;
    SolSettings         *settings;
    SolBgEffectRegistry *bg_effects;
    Ca_Signal           *bg_effect_revision;
    SolUISystem         *ui;
    Ca_Signal           *theme_revision;

    Ca_Signal    *sig_rev;
    Ca_Div       *content_host;

    int           active_tab;
    SwTabCtx      tab_ctxs[SW_TAB_COUNT];

    char          scale_input_text[16];

    /* (no text buffers needed — appearance controls use sliders) */

    /* Theme select state */
    const char   *theme_names[SW_MAX_THEMES];   /* pointers into registry (stable) */
    char          theme_ids[SW_MAX_THEMES][SOL_THEME_ID_MAX + 1];
    int           theme_count;
    int           theme_selected;              /* committed selection index */
    char          preview_theme_id[SOL_THEME_ID_MAX + 1]; /* snapshot before open */

    /* Style select state (same registry shape as themes) */
    const char   *style_names[SW_MAX_STYLES];   /* pointers into registry (stable) */
    char          style_ids[SW_MAX_STYLES][SOL_THEME_ID_MAX + 1];
    int           style_count;
    int           style_selected;              /* committed selection index */
    char          preview_style_id[SOL_THEME_ID_MAX + 1]; /* snapshot before open */

    /* Effect select state */
    char          effect_names_buf[SW_MAX_EFFECTS][64];
    const char   *effect_names[SW_MAX_EFFECTS]; /* pointers into effect_names_buf */
    char          effect_ids[SW_MAX_EFFECTS][64];
    int           effect_count;
    int           effect_selected;             /* committed selection index */
    char          preview_effect_id[64];       /* snapshot before open */

    /* Preferences */
    char          autosave_delay_text[16];

    /* Keybindings */
    SwBindRow     bind_rows[SW_MAX_BIND_ROWS];
    int           bind_row_count;
    SolConfigBinding defaults[SW_MAX_DEFAULT_BINDINGS];
    size_t        default_count;
    char          bind_status[SW_STATUS_MAX];
    bool          bind_status_error;
    bool          reset_armed;                 /* "Reset all" awaiting confirm */

    SolSettingsWindow *next;
};

/* ------------------------------------------------------------------ */
/* Global singleton list                                               */
/* ------------------------------------------------------------------ */

static SolSettingsWindow *g_sw_windows = NULL;

/* ------------------------------------------------------------------ */
/* Label helpers                                                       */
/* ------------------------------------------------------------------ */

static void sw_update_scale_label(SolSettingsWindow *w)
{
    snprintf(w->scale_input_text, sizeof(w->scale_input_text),
             "%.2f", (double)w->settings->ui_scale);
}



/* ------------------------------------------------------------------ */
/* Theme select table rebuild                                          */
/* ------------------------------------------------------------------ */

/*
 * Refresh the theme names/ids arrays from the live registry and recompute
 * theme_selected to match the active theme.
 *
 * w  Settings window to refresh.
 */
static void sw_rebuild_theme_table(SolSettingsWindow *w)
{
    w->theme_count = 0;
    const char *active = sol_ui_system_active_theme(w->ui);
    w->theme_selected = 0;
    size_t count = sol_ui_system_theme_count(w->ui);
    for (size_t i = 0; i < count && i < SW_MAX_THEMES; ++i) {
        const char *id = NULL, *name = NULL;
        if (!sol_ui_system_theme_info(w->ui, i, &id, &name) || !id || !name) continue;
        snprintf(w->theme_ids[w->theme_count], sizeof(w->theme_ids[0]), "%s", id);
        w->theme_names[w->theme_count] = name;
        if (active && strcmp(active, id) == 0)
            w->theme_selected = w->theme_count;
        ++w->theme_count;
    }
}

/* ------------------------------------------------------------------ */
/* Style select table rebuild                                          */
/* ------------------------------------------------------------------ */

/*
 * Refresh the style names/ids arrays from the live registry and recompute
 * style_selected to match the active style.
 *
 * w  Settings window to refresh.
 */
static void sw_rebuild_style_table(SolSettingsWindow *w)
{
    w->style_count = 0;
    const char *active = sol_ui_system_active_style(w->ui);
    w->style_selected = 0;
    size_t count = sol_ui_system_style_count(w->ui);
    for (size_t i = 0; i < count && i < SW_MAX_STYLES; ++i) {
        const char *id = NULL, *name = NULL;
        if (!sol_ui_system_style_info(w->ui, i, &id, &name) || !id || !name) continue;
        snprintf(w->style_ids[w->style_count], sizeof(w->style_ids[0]), "%s", id);
        w->style_names[w->style_count] = name;
        if (active && strcmp(active, id) == 0)
            w->style_selected = w->style_count;
        ++w->style_count;
    }
}

/* ------------------------------------------------------------------ */
/* Effect select table rebuild                                         */
/* ------------------------------------------------------------------ */

/*
 * Refresh the effect names/ids arrays from the live registry and recompute
 * effect_selected to match the active effect.
 *
 * w  Settings window to refresh.
 */
static void sw_rebuild_effect_table(SolSettingsWindow *w)
{
    w->effect_count = 0;
    if (!w->bg_effects) return;

    /* Slot 0 = None */
    snprintf(w->effect_names_buf[0], sizeof(w->effect_names_buf[0]), "None");
    w->effect_names[0] = w->effect_names_buf[0];
    w->effect_ids[0][0] = '\0';
    w->effect_count = 1;
    w->effect_selected = 0;

    const char *active_id = sol_bg_effect_active_id(w->bg_effects);
    size_t count = sol_bg_effect_count(w->bg_effects);
    for (size_t i = 0; i < count && w->effect_count < SW_MAX_EFFECTS; ++i) {
        const char *id = NULL, *name = NULL;
        if (!sol_bg_effect_get_info(w->bg_effects, i, &id, &name) || !id || !name) continue;
        snprintf(w->effect_names_buf[w->effect_count], sizeof(w->effect_names_buf[0]),
                 "%s", name);
        w->effect_names[w->effect_count] = w->effect_names_buf[w->effect_count];
        snprintf(w->effect_ids[w->effect_count], sizeof(w->effect_ids[0]), "%s", id);
        if (active_id && strcmp(active_id, id) == 0)
            w->effect_selected = w->effect_count;
        ++w->effect_count;
    }
}

/* ------------------------------------------------------------------ */
/* Tab callbacks                                                       */
/* ------------------------------------------------------------------ */

/*
 * Handle a tab-button click: switch to the selected tab and bump sig_rev.
 *
 * btn        Clicked button (unused).
 * user_data  SwTabCtx pointer.
 */
static void sw_on_tab_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SwTabCtx          *ctx = (SwTabCtx *)user_data;
    SolSettingsWindow *w   = ctx->win;
    if (w->active_tab == ctx->tab_index) return;
    w->active_tab = ctx->tab_index;
    sol_ui_bump_u32(w->sig_rev);
}

/* ------------------------------------------------------------------ */
/* Theme select callbacks                                              */
/* ------------------------------------------------------------------ */

/*
 * Called each time the highlighted item in the theme dropdown changes.
 * idx ≥ 0: apply hovered theme as live preview.
 * idx = -1: dropdown closed without commit — revert to snapshot.
 *
 * sel        The select widget.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_theme_hover(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    int idx = ca_select_get_hover(sel);
    if (idx < 0) {
        /* Dropdown dismissed without a commit — revert to pre-open snapshot. */
        if (w->preview_theme_id[0] != '\0')
            sol_ui_system_set_active_theme(w->ui, w->preview_theme_id);
        return;
    }
    if (idx >= w->theme_count) return;
    sol_ui_system_set_active_theme(w->ui, w->theme_ids[idx]);
}

/*
 * Called when the user clicks to commit a theme selection.
 * Applies, persists, and updates the committed snapshot.
 *
 * sel        The select widget.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_theme_change(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    int idx = ca_select_get(sel);
    if (idx < 0 || idx >= w->theme_count) return;
    w->theme_selected = idx;
    snprintf(w->preview_theme_id, sizeof(w->preview_theme_id), "%s", w->theme_ids[idx]);
    sol_ui_system_set_active_theme(w->ui, w->theme_ids[idx]);
    snprintf(w->settings->theme_id, sizeof(w->settings->theme_id), "%s", w->theme_ids[idx]);
    sol_settings_save(w->settings);
    sol_ui_bump_u32(w->sig_rev);
}


/* ------------------------------------------------------------------ */
/* Style select callbacks                                              */
/* ------------------------------------------------------------------ */

/*
 * Called each time the highlighted item in the style dropdown changes.
 * idx >= 0: apply hovered style as live preview.
 * idx = -1: dropdown closed without commit — revert to snapshot.
 *
 * sel        The select widget.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_style_hover(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    int idx = ca_select_get_hover(sel);
    if (idx < 0) {
        if (w->preview_style_id[0] != '\0')
            sol_ui_system_set_active_style(w->ui, w->preview_style_id);
        return;
    }
    if (idx >= w->style_count) return;
    sol_ui_system_set_active_style(w->ui, w->style_ids[idx]);
}

/*
 * Called when the user clicks to commit a style selection.
 * Applies, persists, and updates the committed snapshot.
 *
 * sel        The select widget.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_style_change(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    int idx = ca_select_get(sel);
    if (idx < 0 || idx >= w->style_count) return;
    w->style_selected = idx;
    snprintf(w->preview_style_id, sizeof(w->preview_style_id), "%s", w->style_ids[idx]);
    sol_ui_system_set_active_style(w->ui, w->style_ids[idx]);
    snprintf(w->settings->style_id, sizeof(w->settings->style_id), "%s", w->style_ids[idx]);
    sol_settings_save(w->settings);
    sol_ui_bump_u32(w->sig_rev);
}

/* ------------------------------------------------------------------ */
/* Effect select callbacks                                             */
/* ------------------------------------------------------------------ */

/*
 * Called each time the highlighted item in the effect dropdown changes.
 * idx ≥ 0: activate hovered effect as live preview.
 * idx = -1: dropdown closed without commit — revert to snapshot.
 *
 * sel        The select widget.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_effect_hover(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    if (!w->bg_effects) return;
    int idx = ca_select_get_hover(sel);
    if (idx < 0) {
        /* Dropdown dismissed without commit — revert to pre-open snapshot. */
        const char *want = w->preview_effect_id[0] != '\0' ? w->preview_effect_id : NULL;
        sol_bg_effect_set_active(w->bg_effects, want);
        return;
    }
    if (idx >= w->effect_count) return;
    const char *id = w->effect_ids[idx][0] != '\0' ? w->effect_ids[idx] : NULL;
    sol_bg_effect_set_active(w->bg_effects, id);
}

/*
 * Called when the user clicks to commit an effect selection.
 * Activates, persists, and updates the committed snapshot.
 *
 * sel        The select widget.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_effect_change(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    if (!w->bg_effects) return;
    int idx = ca_select_get(sel);
    if (idx < 0 || idx >= w->effect_count) return;
    w->effect_selected = idx;
    snprintf(w->preview_effect_id, sizeof(w->preview_effect_id), "%s", w->effect_ids[idx]);
    sol_bg_effect_set_active(w->bg_effects, w->effect_ids[idx]);

    const char *active = sol_bg_effect_active_id(w->bg_effects);
    if (active)
        snprintf(w->settings->bg_effect_id, sizeof(w->settings->bg_effect_id), "%s", active);
    else
        w->settings->bg_effect_id[0] = '\0';
    sol_settings_save(w->settings);
    sol_ui_bump_u32(w->sig_rev);
}

/* ------------------------------------------------------------------ */
/* Input callbacks                                                     */
/* ------------------------------------------------------------------ */

/*
 * Handle UI-scale text input change.  Parses, clamps, applies, and persists.
 *
 * inp        Text input node.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_scale_input_change(Ca_TextInput *inp, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    const char *text = ca_get_text(inp);
    if (!text) return;
    snprintf(w->scale_input_text, sizeof(w->scale_input_text), "%s", text);
    char *end;
    float val = strtof(text, &end);
    if (end == text || *end != '\0') return;
    if (val < SOL_SETTINGS_UI_SCALE_MIN || val > SOL_SETTINGS_UI_SCALE_MAX) return;
    w->settings->ui_scale = val;
    ca_instance_set_scale(w->instance, val);
    sol_settings_save(w->settings);
}

static void sw_on_opacity_change(Ca_Slider *sl, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    w->settings->bg_opacity = ca_slider_get(sl);
    if (w->bg_effects) sol_bg_effect_set_opacity(w->bg_effects, w->settings->bg_opacity);
    sol_settings_save(w->settings);
}

/* ------------------------------------------------------------------ */
/* Appearance overlay callbacks (sliders)                             */
/* ------------------------------------------------------------------ */

#define SW_MAKE_SLIDER_CB(fn_name, field)                           \
static void fn_name(Ca_Slider *sl, void *user_data)                 \
{                                                                    \
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;           \
    w->settings->field = ca_slider_get(sl);                         \
    sol_ui_system_apply_appearance(w->ui);                           \
    sol_settings_save(w->settings);                                  \
}

SW_MAKE_SLIDER_CB(sw_on_corner_radius_change,    corner_radius)
SW_MAKE_SLIDER_CB(sw_on_panel_blur_change,       panel_blur)
SW_MAKE_SLIDER_CB(sw_on_titlebar_blur_change,    titlebar_blur)
SW_MAKE_SLIDER_CB(sw_on_panel_opacity_change,    panel_opacity)
SW_MAKE_SLIDER_CB(sw_on_scrollbar_width_change,  scrollbar_width)

#undef SW_MAKE_SLIDER_CB

/* ------------------------------------------------------------------ */
/* Preferences callbacks                                               */
/* ------------------------------------------------------------------ */

/*
 * Store a boolean preference, persist it, and push it into the live UI.
 *
 * w      Settings window.
 * field  Preference field inside w->settings.
 * value  New value.
 */
static void sw_set_preference_flag(SolSettingsWindow *w, bool *field, bool value)
{
    if (*field == value) return;
    *field = value;
    sol_settings_save(w->settings);
    sol_ui_system_apply_preferences(w->ui);
}

/* Autosave toggle changed. */
static void sw_on_autosave_toggle(Ca_Toggle *t, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    sw_set_preference_flag(w, &w->settings->autosave_enabled, ca_toggle_get(t));
}

/* Caret-blink toggle changed. */
static void sw_on_caret_blink_toggle(Ca_Toggle *t, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    sw_set_preference_flag(w, &w->settings->caret_blink, ca_toggle_get(t));
}

/* Hidden-files toggle changed. */
static void sw_on_hidden_files_toggle(Ca_Toggle *t, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    sw_set_preference_flag(w, &w->settings->show_hidden_files, ca_toggle_get(t));
}

/* Rewrite the autosave-delay input text from the stored preference. */
static void sw_update_autosave_delay_label(SolSettingsWindow *w)
{
    snprintf(w->autosave_delay_text, sizeof(w->autosave_delay_text),
             "%.2f", (double)w->settings->autosave_delay);
}

/*
 * Autosave-delay input edited. Applies and persists only a complete,
 * in-range number, so partial input such as "2." is left alone.
 *
 * inp        Text input node.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_autosave_delay_change(Ca_TextInput *inp, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    const char *text = ca_get_text(inp);
    if (!text) return;
    snprintf(w->autosave_delay_text, sizeof(w->autosave_delay_text), "%s", text);
    char *end = NULL;
    const float val = strtof(text, &end);
    if (end == text || *end != '\0') return;
    if (!(val >= SOL_SETTINGS_AUTOSAVE_DELAY_MIN && val <= SOL_SETTINGS_AUTOSAVE_DELAY_MAX)) return;
    if (val == w->settings->autosave_delay) return;
    w->settings->autosave_delay = val;
    sol_settings_save(w->settings);
}

/*
 * Emit one "label — toggle — hint" preference row.
 *
 * w      Settings window (callback data).
 * label  Row label.
 * on     Current value.
 * cb     Toggle callback.
 * hint   Muted explanatory text (may be NULL).
 */
static void sw_toggle_row(SolSettingsWindow *w, const char *label, bool on,
                          Ca_ToggleFn cb, const char *hint)
{
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = label, .style = "sw-setting-label sw-setting-label-wide" });
    ca_toggle(&(Ca_ToggleDesc){ .on = on, .on_change = cb, .change_data = w });
    if (hint) ca_text(&(Ca_TextDesc){ .text = hint, .style = "sw-setting-hint" });
    ca_div_end();
}

/*
 * Emit the Preferences tab.
 *
 * w  Settings window providing state and callbacks.
 */
static void sw_render_preferences_tab(SolSettingsWindow *w)
{
    (void)ca_signal_get_u32(w->ui->sig_prefs_rev);
    const SolSettings *st = w->settings;

    char *end = NULL;
    const float shown = strtof(w->autosave_delay_text, &end);
    if (end == w->autosave_delay_text || *end != '\0' || shown != st->autosave_delay)
        sw_update_autosave_delay_label(w);

    ca_text(&(Ca_TextDesc){ .text = "EDITOR", .style = "sw-section-title" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    sw_toggle_row(w, "Autosave", st->autosave_enabled, sw_on_autosave_toggle,
                  "Save modified buffers after you stop typing");

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Autosave delay", .style = "sw-setting-label sw-setting-label-wide" });
    ca_input(&(Ca_InputDesc){
        .text        = w->autosave_delay_text,
        .placeholder = "1.50",
        .on_change   = sw_on_autosave_delay_change,
        .change_data = w,
        .disabled    = !st->autosave_enabled,
        .style       = "sw-scale-input",
    });
    ca_text(&(Ca_TextDesc){ .text = "seconds (0.5 – 10)", .style = "sw-setting-hint" });
    ca_div_end();

    sw_toggle_row(w, "Caret blink", st->caret_blink, sw_on_caret_blink_toggle,
                  "A steady caret also saves a redraw every frame");

    ca_text(&(Ca_TextDesc){ .text = "EXPLORER", .style = "sw-section-title sw-section-title-spaced" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    sw_toggle_row(w, "Hidden files", st->show_hidden_files, sw_on_hidden_files_toggle,
                  "Show dotfiles in the explorer and file pickers");
}

/* ------------------------------------------------------------------ */
/* Keybindings model                                                   */
/* ------------------------------------------------------------------ */

/*
 * Set the Keybindings status line.
 *
 * w      Settings window.
 * error  true to style the message as an error.
 * fmt    printf-style format.
 */
static void sw_bind_status(SolSettingsWindow *w, bool error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->bind_status, sizeof(w->bind_status), fmt, ap);
    va_end(ap);
    w->bind_status_error = error;
    sol_ui_bump_u32(w->sig_rev);
}

/* Return the default chord text for action, or "" when it has none. */
static const char *sw_default_chord(const SolSettingsWindow *w, const char *action)
{
    for (size_t i = 0u; i < w->default_count; ++i) {
        if (strcmp(w->defaults[i].action, action) == 0) return w->defaults[i].chord;
    }
    return "";
}

/* Find the row for action, appending an empty one when absent and room remains. */
static SwBindRow *sw_bind_row(SolSettingsWindow *w, const char *action)
{
    for (int i = 0; i < w->bind_row_count; ++i) {
        if (strcmp(w->bind_rows[i].action, action) == 0) return &w->bind_rows[i];
    }
    if (w->bind_row_count >= SW_MAX_BIND_ROWS) return NULL;
    SwBindRow *row = &w->bind_rows[w->bind_row_count++];
    memset(row, 0, sizeof(*row));
    row->win = w;
    snprintf(row->action, sizeof(row->action), "%s", action);
    return row;
}

/* Point a row at a new committed chord, discarding its draft only on change. */
static void sw_bind_row_commit(SwBindRow *row, const char *chord)
{
    if (strcmp(row->committed, chord) == 0) return;
    snprintf(row->committed, sizeof(row->committed), "%s", chord);
    snprintf(row->draft, sizeof(row->draft), "%s", chord);
}

/* qsort comparator ordering rows by action name. */
static int sw_bind_row_cmp(const void *a, const void *b)
{
    return strcmp(((const SwBindRow *)a)->action, ((const SwBindRow *)b)->action);
}

/*
 * Reconcile the rows with the live registry and the default table. Every
 * registered action and every default action gets a row; rows for actions
 * that became unbound this session are kept so they can be rebound.
 *
 * w  Settings window.
 */
static void sw_sync_bind_rows(SolSettingsWindow *w)
{
    for (int i = 0; i < w->bind_row_count; ++i) w->bind_rows[i].seen = false;

    for (size_t i = 0u; i < w->ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &w->ui->command_flows[i];
        SwBindRow *row = sw_bind_row(w, flow->action);
        if (!row) continue;
        char chord[SW_CHORD_MAX] = "";
        if (flow->sequence_length > 0u &&
            !sol_config_format_chord(flow->sequence, flow->step_modifiers,
                                     flow->sequence_length, chord, sizeof(chord))) {
            snprintf(chord, sizeof(chord), "?");
        }
        sw_bind_row_commit(row, chord);
        row->bound = flow->sequence_length > 0u;
        row->seen  = true;
        row->fallback[0] = '\0';
        if (flow->owned && flow->default_length > 0u) {
            (void)sol_config_format_chord(flow->default_sequence, flow->default_modifiers,
                                          flow->default_length, row->fallback,
                                          sizeof(row->fallback));
        }
    }
    for (size_t i = 0u; i < w->default_count; ++i) {
        (void)sw_bind_row(w, w->defaults[i].action);
    }
    for (int i = 0; i < w->bind_row_count; ++i) {
        SwBindRow *row = &w->bind_rows[i];
        if (row->fallback[0] == '\0' || !row->seen) {
            snprintf(row->fallback, sizeof(row->fallback), "%s", sw_default_chord(w, row->action));
        }
        if (!row->seen) {
            row->bound = false;
            sw_bind_row_commit(row, "");
        }
    }
    qsort(w->bind_rows, (size_t)w->bind_row_count, sizeof(w->bind_rows[0]), sw_bind_row_cmp);
}

/*
 * Check a chord against every other registered flow. Equal chords and
 * chords where one is a prefix of the other are both rejected: the
 * shorter one would fire before the longer could ever be completed.
 *
 * w         Settings window.
 * action    Action being rebound (its own current chord is ignored).
 * sequence  Candidate key steps.
 * mods      Candidate per-step modifiers.
 * length    Step count.
 * Returns   The conflicting flow, or NULL when the chord is free.
 */
static const SolCommandFlowBinding *sw_chord_conflict(const SolSettingsWindow *w,
                                                      const char *action,
                                                      const SolKeyCode *sequence,
                                                      const SolModifierMask *mods,
                                                      size_t length)
{
    for (size_t i = 0u; i < w->ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &w->ui->command_flows[i];
        if (flow->sequence_length == 0u || strcmp(flow->action, action) == 0) continue;
        const size_t common = flow->sequence_length < length ? flow->sequence_length : length;
        bool same = true;
        for (size_t k = 0u; k < common && same; ++k) {
            same = flow->sequence[k] == sol_ui_normalize_flow_key(sequence[k]) &&
                   flow->step_modifiers[k] == mods[k];
        }
        if (same) return flow;
    }
    return NULL;
}

/*
 * Ask the host to reload the keymap from bindings.conf immediately.
 * Other projects and Sol instances follow through their config watchers.
 *
 * w  Settings window.
 */
static void sw_publish_bindings_changed(SolSettingsWindow *w)
{
    SolEventBus *bus = w->ui->buffers ? sol_buffer_event_bus(w->ui->buffers) : NULL;
    sol_event_publish(bus, SOL_EVENT_BINDINGS_CHANGED, NULL, 0u, w->ui);
}

/*
 * Validate, persist, and apply a chord for a row. Empty text unbinds.
 *
 * w      Settings window.
 * row    Row being edited.
 * chord  Leader-relative chord text.
 */
static void sw_apply_binding(SolSettingsWindow *w, SwBindRow *row, const char *chord)
{
    w->reset_armed = false;
    char action[SOL_UI_MAX_ACTION_LEN + 1u];
    snprintf(action, sizeof(action), "%s", row->action);
    const char *leader = SW_LEADER_NAMES[0];
    for (int i = 0; i < SW_LEADER_COUNT; ++i) {
        if (SW_LEADER_MODS[i] == w->ui->leader_modifier) leader = SW_LEADER_NAMES[i];
    }

    size_t start = 0u;
    while (chord[start] == ' ' || chord[start] == '\t') ++start;
    if (chord[start] == '\0') {
        if (!row->bound) {
            sw_bind_status(w, false, "%s is already unbound.", action);
            return;
        }
        if (!sol_config_save_binding(action, NULL, NULL, 0u)) {
            sw_bind_status(w, true, "Could not write bindings.conf.");
            return;
        }
        sw_publish_bindings_changed(w);
        sw_bind_status(w, false, "%s unbound.", action);
        return;
    }

    SolKeyCode      sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    SolModifierMask mods[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t          length = 0u;
    if (!sol_config_parse_chord(chord, w->ui->leader_modifier, sequence, mods, &length)) {
        sw_bind_status(w, true,
                       "\"%s\" is not a valid chord. Use keys after the leader, e.g. \"b shift+s\".",
                       chord + start);
        return;
    }

    char canonical[SW_CHORD_MAX];
    if (!sol_config_format_chord(sequence, mods, length, canonical, sizeof(canonical))) {
        sw_bind_status(w, true, "\"%s\" cannot be written to bindings.conf.", chord + start);
        return;
    }
    if (row->bound && strcmp(canonical, row->committed) == 0) {
        snprintf(row->draft, sizeof(row->draft), "%s", canonical);
        sw_bind_status(w, false, "%s is already bound to %s %s.", action, leader, canonical);
        return;
    }

    const SolCommandFlowBinding *clash = sw_chord_conflict(w, action, sequence, mods, length);
    if (clash) {
        char other[SW_CHORD_MAX] = "?";
        (void)sol_config_format_chord(clash->sequence, clash->step_modifiers,
                                      clash->sequence_length, other, sizeof(other));
        sw_bind_status(w, true, "%s %s conflicts with %s (%s %s).",
                       leader, canonical, clash->action, leader, other);
        return;
    }

    if (!sol_config_save_binding(action, sequence, mods, length)) {
        sw_bind_status(w, true, "Could not write bindings.conf.");
        return;
    }
    sw_publish_bindings_changed(w);
    sw_bind_status(w, false, "%s bound to %s %s.", action, leader, canonical);
}

/* ------------------------------------------------------------------ */
/* Keybindings callbacks                                               */
/* ------------------------------------------------------------------ */

/* Chord input edited: keep the draft only; nothing applies until Set. */
static void sw_on_bind_input_change(Ca_TextInput *inp, void *user_data)
{
    SwBindRow *row = (SwBindRow *)user_data;
    const char *text = ca_get_text(inp);
    snprintf(row->draft, sizeof(row->draft), "%s", text ? text : "");
}

/* "Set" clicked: apply the row's draft. */
static void sw_on_bind_set_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SwBindRow *row = (SwBindRow *)user_data;
    char draft[SW_CHORD_MAX];
    snprintf(draft, sizeof(draft), "%s", row->draft);
    sw_apply_binding(row->win, row, draft);
}

/* "Default" clicked: restore the row's built-in chord. */
static void sw_on_bind_default_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SwBindRow *row = (SwBindRow *)user_data;
    char fallback[SW_CHORD_MAX];
    snprintf(fallback, sizeof(fallback), "%s", row->fallback);
    if (fallback[0] == '\0') return;
    sw_apply_binding(row->win, row, fallback);
}

/* "Clear" clicked: unbind the row's action. */
static void sw_on_bind_clear_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SwBindRow *row = (SwBindRow *)user_data;
    sw_apply_binding(row->win, row, "");
}

/* Leader modifier changed: persist it and reload the keymap. */
static void sw_on_leader_change(Ca_Select *sel, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    const int idx = ca_select_get(sel);
    if (idx < 0 || idx >= SW_LEADER_COUNT) return;
    const SolModifierMask previous = w->ui->leader_modifier;
    const SolModifierMask next = SW_LEADER_MODS[idx];
    if (previous == next) return;
    w->reset_armed = false;

    for (size_t i = 0u; i < w->ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &w->ui->command_flows[i];
        for (size_t k = 0u; k < flow->sequence_length; ++k) {
            if ((flow->step_modifiers[k] & next) == 0u) continue;
            sw_bind_status(w, true, "%s cannot lead: %s already uses it inside its chord.",
                           SW_LEADER_NAMES[idx], flow->action);
            return;
        }
    }
    if (!sol_config_save_leader(previous, next)) {
        sw_bind_status(w, true, "Could not write bindings.conf.");
        return;
    }
    sw_publish_bindings_changed(w);
    sw_bind_status(w, false, "Leader key set to %s.", SW_LEADER_NAMES[idx]);
}

/* "Reset all" clicked: first click arms, second click restores defaults. */
static void sw_on_bind_reset_all_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    if (!w->reset_armed) {
        w->reset_armed = true;
        sw_bind_status(w, false,
                       "Click \"Confirm reset\" to replace bindings.conf with the defaults.");
        return;
    }
    w->reset_armed = false;
    if (!sol_config_reset_bindings()) {
        sw_bind_status(w, true, "Could not write bindings.conf.");
        return;
    }
    sw_publish_bindings_changed(w);
    sw_bind_status(w, false, "All keybindings restored to defaults.");
}

/* ------------------------------------------------------------------ */
/* Keybindings view                                                    */
/* ------------------------------------------------------------------ */

/*
 * Emit one action row: name, leader badge, chord input, and actions.
 *
 * w       Settings window.
 * row     Row model.
 * leader  Display name of the leader key.
 */
static void sw_render_bind_row(SolSettingsWindow *w, SwBindRow *row, const char *leader)
{
    (void)w;
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-bind-row" });
    ca_text(&(Ca_TextDesc){ .text = row->action, .style = "sw-bind-action" });
    ca_text(&(Ca_TextDesc){ .text = leader, .style = "sw-bind-leader" });
    ca_input(&(Ca_InputDesc){
        .text        = row->draft,
        .placeholder = "unbound",
        .on_change   = sw_on_bind_input_change,
        .change_data = row,
        .style       = "sw-bind-input",
    });
    ca_btn_begin(&(Ca_BtnDesc){
        .direction = CA_HORIZONTAL, .style = "sw-btn",
        .on_click = sw_on_bind_set_click, .click_data = row,
    });
    ca_text(&(Ca_TextDesc){ .text = "Set", .style = "sw-btn-label" });
    ca_btn_end();
    const bool can_default = row->fallback[0] != '\0' &&
                             (!row->bound || strcmp(row->fallback, row->committed) != 0);
    ca_btn_begin(&(Ca_BtnDesc){
        .direction = CA_HORIZONTAL, .style = "sw-btn", .disabled = !can_default,
        .on_click = sw_on_bind_default_click, .click_data = row,
    });
    ca_text(&(Ca_TextDesc){ .text = "Default", .style = "sw-btn-label" });
    ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){
        .direction = CA_HORIZONTAL, .style = "sw-btn", .disabled = !row->bound,
        .on_click = sw_on_bind_clear_click, .click_data = row,
    });
    ca_text(&(Ca_TextDesc){ .text = "Clear", .style = "sw-btn-label" });
    ca_btn_end();
    ca_div_end();
}

/*
 * Emit the Keybindings tab: leader selector, status line, and a scrolling
 * list of action rows grouped by their dotted prefix.
 *
 * w  Settings window providing state and callbacks.
 */
static void sw_render_keybindings_tab(SolSettingsWindow *w)
{
    (void)ca_signal_get_u32(w->ui->sig_flow_registry_rev);
    sw_sync_bind_rows(w);

    int leader_idx = 0;
    for (int i = 0; i < SW_LEADER_COUNT; ++i) {
        if (SW_LEADER_MODS[i] == w->ui->leader_modifier) leader_idx = i;
    }
    const char *leader = SW_LEADER_NAMES[leader_idx];

    ca_text(&(Ca_TextDesc){ .text = "KEYBINDINGS", .style = "sw-section-title" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Leader key", .style = "sw-setting-label sw-setting-label-wide" });
    ca_select(&(Ca_SelectDesc){
        .options      = SW_LEADER_NAMES,
        .option_count = SW_LEADER_COUNT,
        .selected     = leader_idx,
        .on_change    = sw_on_leader_change,
        .change_data  = w,
        .style        = "sw-select sw-select-narrow",
    });
    ca_btn_begin(&(Ca_BtnDesc){
        .direction = CA_HORIZONTAL, .style = "sw-btn",
        .on_click = sw_on_bind_reset_all_click, .click_data = w,
    });
    ca_text(&(Ca_TextDesc){
        .text = w->reset_armed ? "Confirm reset" : "Reset all", .style = "sw-btn-label",
    });
    ca_btn_end();
    ca_div_end();

    ca_text(&(Ca_TextDesc){
        .text  = w->bind_status[0] ? w->bind_status
                                   : "Type the keys pressed after the leader, e.g. \"b shift+s\", then Set.",
        .wrap  = true,
        .style = w->bind_status_error ? "sw-bind-status sw-bind-status-error" : "sw-bind-status",
    });

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-scroll" });
    char group[SOL_UI_MAX_ACTION_LEN + 1u] = "";
    for (int i = 0; i < w->bind_row_count; ++i) {
        SwBindRow *row = &w->bind_rows[i];
        const char *dot = strchr(row->action, '.');
        const size_t glen = dot ? (size_t)(dot - row->action) : strlen(row->action);
        if (strlen(group) != glen || strncmp(group, row->action, glen) != 0) {
            snprintf(group, sizeof(group), "%.*s", (int)glen, row->action);
            char title[SOL_UI_MAX_ACTION_LEN + 1u];
            for (size_t k = 0u; k <= glen; ++k)
                title[k] = (char)toupper((unsigned char)group[k]);
            ca_text(&(Ca_TextDesc){ .text = title, .style = "sw-bind-group" });
        }
        sw_render_bind_row(w, row, leader);
    }
    ca_div_end();
}

/* ------------------------------------------------------------------ */
/* Content builder                                                     */
/* ------------------------------------------------------------------ */

/*
 * Emit the Theme settings tab: theme dropdown, scale row, effect dropdown,
 * and intensity row.
 *
 * w  Settings window providing state and callbacks.
 */
static void sw_render_theme_tab(SolSettingsWindow *w)
{
    if (w->theme_revision)      (void)ca_signal_get_u32(w->theme_revision);
    if (w->bg_effect_revision)  (void)ca_signal_get_u32(w->bg_effect_revision);

    /* Rebuild tables each render so they reflect any plugin-driven changes. */
    sw_rebuild_theme_table(w);
    sw_rebuild_style_table(w);

    ca_text(&(Ca_TextDesc){ .text = "THEME", .style = "sw-section-title" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    /* ---- Theme selector ---- */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-setting-group" });
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Theme", .style = "sw-setting-label" });

    ca_select(&(Ca_SelectDesc){
        .options      = w->theme_names,
        .option_count = w->theme_count,
        .selected     = w->theme_selected,
        .on_change    = sw_on_theme_change,
        .change_data  = w,
        .on_hover     = sw_on_theme_hover,
        .hover_data   = w,
        .style        = "sw-select",
    });

    ca_div_end();
    ca_div_end();

    /* ---- Style selector ---- */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-setting-group" });
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Style", .style = "sw-setting-label" });

    ca_select(&(Ca_SelectDesc){
        .options      = w->style_names,
        .option_count = w->style_count,
        .selected     = w->style_selected,
        .on_change    = sw_on_style_change,
        .change_data  = w,
        .on_hover     = sw_on_style_hover,
        .hover_data   = w,
        .style        = "sw-select",
    });

    ca_div_end();
    ca_div_end();

    /* ---- Scale row ---- */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Scale", .style = "sw-setting-label" });
    ca_input(&(Ca_InputDesc){
        .text        = w->scale_input_text,
        .placeholder = "1.00",
        .on_change   = sw_on_scale_input_change,
        .change_data = w,
        .style       = "sw-scale-input",
    });
    ca_text(&(Ca_TextDesc){ .text = "0.5 – 3.0", .style = "sw-setting-value" });
    ca_div_end();

    if (!w->bg_effects) return;

    /* ---- Effect selector ---- */
    sw_rebuild_effect_table(w);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-setting-group" });
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Background", .style = "sw-setting-label" });

    ca_select(&(Ca_SelectDesc){
        .options      = w->effect_names,
        .option_count = w->effect_count,
        .selected     = w->effect_selected,
        .on_change    = sw_on_effect_change,
        .change_data  = w,
        .on_hover     = sw_on_effect_hover,
        .hover_data   = w,
        .style        = "sw-select",
    });

    ca_div_end();
    ca_div_end();

    /* ---- Intensity row ---- */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Intensity", .style = "sw-setting-label" });
    ca_slider(&(Ca_SliderDesc){
        .min         = SOL_SETTINGS_BG_OPACITY_MIN,
        .max         = SOL_SETTINGS_BG_OPACITY_MAX,
        .value       = w->settings->bg_opacity,
        .on_change   = sw_on_opacity_change,
        .change_data = w,
        .style       = "sw-slider",
    });
    ca_div_end();

    /* ---- Appearance section ---- */
    ca_text(&(Ca_TextDesc){ .text = "APPEARANCE", .style = "sw-section-title" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

#define SW_SLIDER_ROW(label, mn, mx, field_val, cb) \
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" }); \
    ca_text(&(Ca_TextDesc){ .text = (label), .style = "sw-setting-label" }); \
    ca_slider(&(Ca_SliderDesc){ \
        .min         = (mn), \
        .max         = (mx), \
        .value       = (field_val), \
        .on_change   = (cb), \
        .change_data = w, \
        .style       = "sw-slider", \
    }); \
    ca_div_end();

    SW_SLIDER_ROW("Corner Radius",   SOL_SETTINGS_CORNER_RADIUS_MIN,   SOL_SETTINGS_CORNER_RADIUS_MAX,   w->settings->corner_radius,   sw_on_corner_radius_change)
    SW_SLIDER_ROW("Panel Opacity",   SOL_SETTINGS_PANEL_OPACITY_MIN,   SOL_SETTINGS_PANEL_OPACITY_MAX,   w->settings->panel_opacity,   sw_on_panel_opacity_change)
    SW_SLIDER_ROW("Panel Blur",      SOL_SETTINGS_PANEL_BLUR_MIN,      SOL_SETTINGS_PANEL_BLUR_MAX,      w->settings->panel_blur,      sw_on_panel_blur_change)
    SW_SLIDER_ROW("Titlebar Blur",   SOL_SETTINGS_TITLEBAR_BLUR_MIN,   SOL_SETTINGS_TITLEBAR_BLUR_MAX,   w->settings->titlebar_blur,   sw_on_titlebar_blur_change)
    SW_SLIDER_ROW("Scrollbar Width", SOL_SETTINGS_SCROLLBAR_WIDTH_MIN, SOL_SETTINGS_SCROLLBAR_WIDTH_MAX, w->settings->scrollbar_width, sw_on_scrollbar_width_change)

#undef SW_SLIDER_ROW
}

/*
 * Reactive builder: subscribes to sig_rev and emits left tab list + active
 * tab right panel.
 *
 * div        The body div being rebuilt (unused directly).
 * user_data  SolSettingsWindow pointer.
 */
static void sw_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    (void)ca_signal_get_u32(w->sig_rev);

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-left" });
    for (int i = 0; i < SW_TAB_COUNT; i++) {
        const bool active = (w->active_tab == i);
        ca_btn_begin(&(Ca_BtnDesc){
            .direction  = CA_HORIZONTAL,
            .style      = active ? "sw-tab-btn sw-tab-btn-active" : "sw-tab-btn",
            .on_click   = sw_on_tab_click,
            .click_data = &w->tab_ctxs[i],
        });
        ca_text(&(Ca_TextDesc){
            .text  = SW_TAB_LABELS[i],
            .style = active ? "sw-tab-label sw-tab-label-active" : "sw-tab-label",
        });
        ca_btn_end();
    }
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-right" });
    switch ((SolUISettingsTab)w->active_tab) {
        case SOL_UI_SETTINGS_TAB_THEME:       sw_render_theme_tab(w);       break;
        case SOL_UI_SETTINGS_TAB_PREFERENCES: sw_render_preferences_tab(w); break;
        case SOL_UI_SETTINGS_TAB_KEYBINDINGS: sw_render_keybindings_tab(w); break;
        default: break;
    }
    ca_div_end();
}

/* ------------------------------------------------------------------ */
/* Window layout                                                       */
/* ------------------------------------------------------------------ */

/*
 * Build the top-level Causality layout for the settings window.
 *
 * w  Settings window to populate.
 */
static void sw_build_layout(SolSettingsWindow *w)
{
    ca_ui_begin(w->window, &(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-root" });
    w->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "sw-body",
    });
    ca_div_set_builder(w->content_host, sw_content_builder, w);
    ca_div_end();
    ca_ui_end();
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

/*
 * Close and free a settings window.  sig_rev is owned by the Causality
 * instance and is not freed here.
 *
 * w  Settings window to destroy (safe to call with NULL).
 */
static void sw_destroy(SolSettingsWindow *w)
{
    if (!w) return;
    if (w->window && ca_window_is_open(w->window))
        ca_window_close(w->window);
    free(w);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Open the Settings window on a tab. When this UI system already has one
 * open, that window is switched to the tab instead.
 *
 * ui   UI system providing settings, registries, and revision signals.
 * tab  Tab to show.
 */
void sol_ui_settings_window_open(SolUISystem *ui, SolUISettingsTab tab)
{
    if (!ui || !ui->instance || !ui->settings) return;
    if ((int)tab < 0 || tab >= SOL_UI_SETTINGS_TAB_COUNT) tab = SOL_UI_SETTINGS_TAB_THEME;

    for (SolSettingsWindow *w = g_sw_windows; w; w = w->next) {
        if (w->ui != ui || !w->window || !ca_window_is_open(w->window)) continue;
        if (w->active_tab != (int)tab) {
            w->active_tab = (int)tab;
            sol_ui_bump_u32(w->sig_rev);
        }
        return;
    }

    SolSettingsWindow *w = (SolSettingsWindow *)calloc(1, sizeof(*w));
    if (!w) return;

    w->instance           = ui->instance;
    w->settings           = ui->settings;
    w->bg_effects         = ui->bg_effects;
    w->bg_effect_revision = ui->sig_bg_effect_rev;
    w->ui                 = ui;
    w->theme_revision     = ui->sig_theme_rev;
    w->active_tab         = (int)tab;

    sw_update_scale_label(w);
    sw_update_autosave_delay_label(w);
    w->default_count = sol_config_default_bindings(w->defaults, SW_MAX_DEFAULT_BINDINGS);
    if (w->default_count > SW_MAX_DEFAULT_BINDINGS) w->default_count = SW_MAX_DEFAULT_BINDINGS;

    for (int i = 0; i < SW_TAB_COUNT; i++) {
        w->tab_ctxs[i].win       = w;
        w->tab_ctxs[i].tab_index = i;
    }

    /* Snapshot committed selections for revert-on-dismiss. */
    const char *active_theme = sol_ui_system_active_theme(ui);
    if (active_theme)
        snprintf(w->preview_theme_id, sizeof(w->preview_theme_id), "%s", active_theme);

    const char *active_style = sol_ui_system_active_style(ui);
    if (active_style)
        snprintf(w->preview_style_id, sizeof(w->preview_style_id), "%s", active_style);

    if (w->bg_effects) {
        const char *active_fx = sol_bg_effect_active_id(w->bg_effects);
        if (active_fx)
            snprintf(w->preview_effect_id, sizeof(w->preview_effect_id), "%s", active_fx);
    }

    w->sig_rev = ca_signal_u32(ui->instance, 0u);
    if (!w->sig_rev) { free(w); return; }

    w->window = ca_window_create(ui->instance, &(Ca_WindowDesc){
        .title  = "Settings",
        .width  = SW_DEFAULT_WIDTH,
        .height = SW_DEFAULT_HEIGHT,
    });
    if (!w->window) { free(w); return; }

    sw_build_layout(w);

    w->next      = g_sw_windows;
    g_sw_windows = w;
}

/*
 * Advance settings-window lifecycle: destroy closed windows.  Call once per frame.
 */
void sol_ui_settings_window_tick(void)
{
    SolSettingsWindow **link = &g_sw_windows;
    while (*link) {
        SolSettingsWindow *w = *link;
        if (!w->window || !ca_window_is_open(w->window)) {
            *link     = w->next;
            w->window = NULL;
            sw_destroy(w);
            continue;
        }
        link = &w->next;
    }
}

/** Destroy auxiliary windows owned by this project before its services disappear. */
void sol_ui_settings_window_close_owner(SolUISystem *ui)
{
    SolSettingsWindow **link = &g_sw_windows;
    while (*link) {
        SolSettingsWindow *w = *link;
        if (w->ui != ui) { link = &w->next; continue; }
        *link = w->next;
        if (w->window && ca_window_is_open(w->window)) ca_window_destroy(w->window);
        w->window = NULL;
        sw_destroy(w);
    }
}
