// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* settings_window.c — Sol Settings window.
 *
 * Two-panel layout (660 × 480):
 *
 *   ┌── sw-left (180px) ──┐  ┌── sw-right (flex-grow) ──────────────────┐
 *   │  [▸ Theme]          │  │  THEME                                   │
 *   │                     │  │  ──────────────────────────────────────  │
 *   │                     │  │  Scale   ──[▓▓▓▓▓▓░░░░]──  1.00×       │
 *   │                     │  │                                          │
 *   │                     │  │  Background Effect                       │
 *   │                     │  │  [ None ] [ Plasma ] [ Aurora ] …       │
 *   │                     │  │                                          │
 *   │                     │  │  Intensity   ──[▓▓░░░░]──  1.00        │
 *   └─────────────────────┘  └──────────────────────────────────────────┘
 *
 * Active tab is stored per-window and drives the reactive builder.
 * Scale and opacity changes are applied immediately and persisted.
 * Effect changes activate the registry effect and persist the id.
 *
 * Follows the same singleton pattern as plugin_window.c: at most one
 * settings window is open at a time; re-opening is a no-op.
 */

#include "sol_ui_internal.h"
#include "sol_bg_effect.h"
#include "sol_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define SW_DEFAULT_WIDTH   660
#define SW_DEFAULT_HEIGHT  480

#define SW_TAB_THEME  0
#define SW_TAB_COUNT  1

static const char * const SW_TAB_LABELS[SW_TAB_COUNT] = { "Theme" };

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolSettingsWindow SolSettingsWindow;

typedef struct {
    SolSettingsWindow *win;
    int                tab_index;
} SwTabCtx;

typedef struct {
    SolSettingsWindow *win;
    char               effect_id[64];  /* empty string = "None" */
} SwEffectCtx;

#define SW_MAX_EFFECT_BTNS 33  /* 1 "None" + up to SOL_BG_EFFECT_MAX */

struct SolSettingsWindow {
    Ca_Window           *window;
    Ca_Instance         *instance;
    SolSettings         *settings;
    SolBgEffectRegistry *bg_effects;
    Ca_Signal           *bg_effect_revision;

    Ca_Signal    *sig_rev;
    Ca_Div       *content_host;

    int           active_tab;
    SwTabCtx      tab_ctxs[SW_TAB_COUNT];

    char          scale_input_text[16];
    char          opacity_input_text[16];

    SwEffectCtx   effect_ctxs[SW_MAX_EFFECT_BTNS];

    SolSettingsWindow *next;
};

/* ------------------------------------------------------------------ */
/* Global singleton list                                               */
/* ------------------------------------------------------------------ */

static SolSettingsWindow *g_sw_windows = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Refresh the scale input text from persisted settings. */
static void sw_update_scale_label(SolSettingsWindow *w)
{
    snprintf(w->scale_input_text, sizeof(w->scale_input_text),
             "%.2f", (double)w->settings->ui_scale);
}

/* Refresh the opacity input text from persisted settings. */
static void sw_update_opacity_label(SolSettingsWindow *w)
{
    snprintf(w->opacity_input_text, sizeof(w->opacity_input_text),
             "%.2f", (double)w->settings->bg_opacity);
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                    */
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

/*
 * Handle an effect button click: activate the effect, persist, and rebuild.
 *
 * btn        Clicked button (unused).
 * user_data  SwEffectCtx pointer.
 */
static void sw_on_effect_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SwEffectCtx       *ctx = (SwEffectCtx *)user_data;
    SolSettingsWindow *w   = ctx->win;
    if (!w->bg_effects) return;

    sol_bg_effect_set_active(w->bg_effects, ctx->effect_id);

    const char *active = sol_bg_effect_active_id(w->bg_effects);
    if (active)
        strncpy(w->settings->bg_effect_id, active,
                sizeof(w->settings->bg_effect_id) - 1);
    else
        w->settings->bg_effect_id[0] = '\0';
    w->settings->bg_effect_id[sizeof(w->settings->bg_effect_id) - 1] = '\0';

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
    char *end;
    float val = strtof(text, &end);
    if (end == text || *end != '\0') return;
    if (val < SOL_SETTINGS_UI_SCALE_MIN || val > SOL_SETTINGS_UI_SCALE_MAX) return;

    w->settings->ui_scale = val;
    ca_instance_set_scale(w->instance, val);
    sol_settings_save(w->settings);
    snprintf(w->scale_input_text, sizeof(w->scale_input_text), "%s", text);
}

/*
 * Handle opacity text input change.  Parses, clamps, applies, and persists.
 *
 * inp        Text input node.
 * user_data  SolSettingsWindow pointer.
 */
static void sw_on_opacity_input_change(Ca_TextInput *inp, void *user_data)
{
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;
    const char *text = ca_get_text(inp);
    if (!text) return;
    char *end;
    float val = strtof(text, &end);
    if (end == text || *end != '\0') return;
    if (val < SOL_SETTINGS_BG_OPACITY_MIN || val > SOL_SETTINGS_BG_OPACITY_MAX) return;

    w->settings->bg_opacity = val;
    if (w->bg_effects) sol_bg_effect_set_opacity(w->bg_effects, val);
    sol_settings_save(w->settings);
    snprintf(w->opacity_input_text, sizeof(w->opacity_input_text), "%s", text);
}

/* ------------------------------------------------------------------ */
/* Content builder                                                     */
/* ------------------------------------------------------------------ */

/*
 * Emit the Theme settings tab: scale row, effect picker, and intensity row.
 *
 * w  Settings window providing state and callbacks.
 */
static void sw_render_theme_tab(SolSettingsWindow *w)
{
    if (w->bg_effect_revision)
        (void)ca_signal_get_u32(w->bg_effect_revision);

    ca_text(&(Ca_TextDesc){ .text = "THEME", .style = "sw-section-title" });
    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    /* Scale row */
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

    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "sw-setting-group" });

    ca_text(&(Ca_TextDesc){ .text = "Background Effect", .style = "sw-setting-label-block" });

    /* Effect selector row */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-effect-row" });

    const char *active_id = sol_bg_effect_active_id(w->bg_effects);
    const bool  none_on   = (active_id == NULL || active_id[0] == '\0');

    ca_btn_begin(&(Ca_BtnDesc){
        .direction  = CA_HORIZONTAL,
        .style      = none_on ? "sw-effect-btn sw-effect-btn-active" : "sw-effect-btn",
        .on_click   = sw_on_effect_click,
        .click_data = &w->effect_ctxs[0],
    });
    ca_text(&(Ca_TextDesc){
        .text  = "None",
        .style = none_on ? "sw-effect-label sw-effect-label-active" : "sw-effect-label",
    });
    ca_btn_end();

    size_t count = sol_bg_effect_count(w->bg_effects);
    for (size_t i = 0; i < count && (i + 1) < SW_MAX_EFFECT_BTNS; ++i) {
        const char *id = NULL, *name = NULL;
        if (!sol_bg_effect_get_info(w->bg_effects, i, &id, &name)) continue;
        w->effect_ctxs[i + 1].win = w;
        snprintf(w->effect_ctxs[i + 1].effect_id,
                 sizeof(w->effect_ctxs[i + 1].effect_id), "%s", id);
        const bool is_on = active_id && id && strcmp(active_id, id) == 0;
        ca_btn_begin(&(Ca_BtnDesc){
            .direction  = CA_HORIZONTAL,
            .style      = is_on ? "sw-effect-btn sw-effect-btn-active" : "sw-effect-btn",
            .on_click   = sw_on_effect_click,
            .click_data = &w->effect_ctxs[i + 1],
        });
        ca_text(&(Ca_TextDesc){
            .text  = name,
            .style = is_on ? "sw-effect-label sw-effect-label-active" : "sw-effect-label",
        });
        ca_btn_end();
    }

    ca_div_end(); /* sw-effect-row */

    /* Intensity row */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "sw-setting-row" });
    ca_text(&(Ca_TextDesc){ .text = "Intensity", .style = "sw-setting-label" });
    ca_input(&(Ca_InputDesc){
        .text        = w->opacity_input_text,
        .placeholder = "1.00",
        .on_change   = sw_on_opacity_input_change,
        .change_data = w,
        .style       = "sw-scale-input",
    });
    ca_text(&(Ca_TextDesc){ .text = "0.0 – 1.0", .style = "sw-setting-value" });
    ca_div_end();

    ca_div_end(); /* sw-setting-group */
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
    switch (w->active_tab) {
        case SW_TAB_THEME: sw_render_theme_tab(w); break;
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
 * Open the Settings window.  No-op if one is already open.
 *
 * instance    Causality instance for window creation.
 * settings    Settings object to read and write.
 * bg_effects  Background effect registry for the effect picker (may be NULL).
 */
void sol_ui_settings_window_open(Ca_Instance *instance, SolSettings *settings,
                                  SolBgEffectRegistry *bg_effects,
                                  Ca_Signal *bg_effect_revision)
{
    if (!instance || !settings) return;

    for (SolSettingsWindow *w = g_sw_windows; w; w = w->next) {
        if (w->window && ca_window_is_open(w->window)) return;
    }

    SolSettingsWindow *w = (SolSettingsWindow *)calloc(1, sizeof(*w));
    if (!w) return;

    w->instance   = instance;
    w->settings   = settings;
    w->bg_effects = bg_effects;
    w->bg_effect_revision = bg_effect_revision;
    w->active_tab = SW_TAB_THEME;

    sw_update_scale_label(w);
    sw_update_opacity_label(w);

    for (int i = 0; i < SW_TAB_COUNT; i++) {
        w->tab_ctxs[i].win       = w;
        w->tab_ctxs[i].tab_index = i;
    }

    /* Effect contexts: slot 0 = None, slots 1..N = registered effects. */
    w->effect_ctxs[0].win          = w;
    w->effect_ctxs[0].effect_id[0] = '\0';
    if (bg_effects) {
        size_t count = sol_bg_effect_count(bg_effects);
        for (size_t i = 0; i < count && (i + 1) < SW_MAX_EFFECT_BTNS; ++i) {
            const char *id = NULL;
            sol_bg_effect_get_info(bg_effects, i, &id, NULL);
            w->effect_ctxs[i + 1].win = w;
            if (id)
                strncpy(w->effect_ctxs[i + 1].effect_id, id,
                        sizeof(w->effect_ctxs[0].effect_id) - 1);
        }
    }

    w->sig_rev = ca_signal_u32(instance, 0u);
    if (!w->sig_rev) { free(w); return; }

    w->window = ca_window_create(instance, &(Ca_WindowDesc){
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
