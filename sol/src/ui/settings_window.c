// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* settings_window.c — Sol Settings window.
 *
 * Two-panel layout (660 × 480):
 *
 *   ┌── sw-left (180px) ──┐  ┌── sw-right (flex-grow) ──────────────────┐
 *   │  [▸ Theme]          │  │  THEME                                   │
 *   │                     │  │  ──────────────────────────────────────  │
 *   │                     │  │  Theme    [Dropdown ▾]                   │
 *   │                     │  │  Scale    [  1.00  ]  0.5 – 3.0         │
 *   │                     │  │                                          │
 *   │                     │  │  Background Effect  [Dropdown ▾]         │
 *   │                     │  │  Intensity   [  1.00  ]  0.0 – 1.0      │
 *   └─────────────────────┘  └──────────────────────────────────────────┘
 *
 * Theme and background effect use ca_select dropdowns.
 * Hovering over an item in the open dropdown live-previews the choice.
 * Clicking outside the dropdown without committing reverts to the
 * previous selection (stored in preview_theme_id / preview_effect_id).
 *
 * Active tab is stored per-window and drives the reactive builder.
 * Scale and opacity changes are applied immediately and persisted.
 * Selections are saved to config and restored at startup.
 *
 * Follows the same singleton pattern as plugin_window.c: at most one
 * settings window is open at a time; re-opening is a no-op.
 */

#include "sol_ui_internal.h"
#include "sol_bg_effect.h"
#include "sol_settings.h"
#include "sol_ui_system.h"

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

#define SW_MAX_EFFECTS  33   /* 1 "None" + up to SOL_BG_EFFECT_MAX */
#define SW_MAX_THEMES   SOL_THEME_MAX

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolSettingsWindow SolSettingsWindow;

typedef struct {
    SolSettingsWindow *win;
    int                tab_index;
} SwTabCtx;

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

    /* Effect select state */
    char          effect_names_buf[SW_MAX_EFFECTS][64];
    const char   *effect_names[SW_MAX_EFFECTS]; /* pointers into effect_names_buf */
    char          effect_ids[SW_MAX_EFFECTS][64];
    int           effect_count;
    int           effect_selected;             /* committed selection index */
    char          preview_effect_id[64];       /* snapshot before open */

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
 * bg_effect_revision  Signal bumped when effects change.
 * ui          Sol UI system owning the theme registry.
 * theme_revision  Signal bumped when themes change.
 */
void sol_ui_settings_window_open(Ca_Instance *instance, SolSettings *settings,
                                  SolBgEffectRegistry *bg_effects,
                                  Ca_Signal *bg_effect_revision,
                                  SolUISystem *ui,
                                  Ca_Signal *theme_revision)
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
    w->ui = ui;
    w->theme_revision = theme_revision;
    w->active_tab = SW_TAB_THEME;

    sw_update_scale_label(w);

    for (int i = 0; i < SW_TAB_COUNT; i++) {
        w->tab_ctxs[i].win       = w;
        w->tab_ctxs[i].tab_index = i;
    }

    /* Snapshot committed selections for revert-on-dismiss. */
    const char *active_theme = sol_ui_system_active_theme(ui);
    if (active_theme)
        snprintf(w->preview_theme_id, sizeof(w->preview_theme_id), "%s", active_theme);

    if (bg_effects) {
        const char *active_fx = sol_bg_effect_active_id(bg_effects);
        if (active_fx)
            snprintf(w->preview_effect_id, sizeof(w->preview_effect_id), "%s", active_fx);
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
