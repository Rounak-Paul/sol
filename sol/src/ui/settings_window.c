// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* settings_window.c — Sol Settings window.
 *
 * Two-panel layout (660 × 440):
 *
 *   ┌── sw-left (180px) ──┐  ┌── sw-right (flex-grow) ──────────────┐
 *   │  [▸ Theme]          │  │  THEME                               │
 *   │                     │  │  ──────────────────────────────────  │
 *   │                     │  │  Scale   ──[▓▓▓▓▓▓░░░░]──  1.00×   │
 *   └─────────────────────┘  └──────────────────────────────────────┘
 *
 * Active tab is stored per-window and drives the reactive builder.
 * Scale changes are applied immediately via ca_instance_set_scale() and
 * persisted to disk via sol_settings_save().
 *
 * Follows the same singleton pattern as plugin_window.c: at most one
 * settings window is open at a time; re-opening is a no-op.
 */

#include "sol_ui_internal.h"
#include "sol_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define SW_DEFAULT_WIDTH   660
#define SW_DEFAULT_HEIGHT  440
#define SW_SCALE_SLIDER_W  220.0f

/* Tab index constants */
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

struct SolSettingsWindow {
    Ca_Window    *window;
    Ca_Instance  *instance;
    SolSettings  *settings;

    Ca_Signal    *sig_rev;    /* u32 — bumped on tab / value change */
    Ca_Div       *content_host;

    int           active_tab;

    /* Stable tab-button click context pool; pointers passed to causality. */
    SwTabCtx      tab_ctxs[SW_TAB_COUNT];

    /* Formatted scale label: "1.00×" — updated on every slider change. */
    char          scale_label[16];

    /* Intrusive list link for lifecycle tracking. */
    SolSettingsWindow *next;
};

/* ------------------------------------------------------------------ */
/* Global singleton list                                               */
/* ------------------------------------------------------------------ */

static SolSettingsWindow *g_sw_windows = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void sw_update_scale_label(SolSettingsWindow *w)
{
    /* × = U+00D7, UTF-8: 0xC3 0x97 */
    snprintf(w->scale_label, sizeof(w->scale_label),
             "%.2f\xc3\x97", (double)w->settings->ui_scale);
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                    */
/* ------------------------------------------------------------------ */

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
/* Slider callbacks                                                    */
/* ------------------------------------------------------------------ */

static void sw_on_scale_change(Ca_Slider *s, void *user_data)
{
    SolSettingsWindow *w   = (SolSettingsWindow *)user_data;
    float              val = ca_slider_get(s);

    /* Clamp to valid range. */
    if (val < SOL_SETTINGS_UI_SCALE_MIN) val = SOL_SETTINGS_UI_SCALE_MIN;
    if (val > SOL_SETTINGS_UI_SCALE_MAX) val = SOL_SETTINGS_UI_SCALE_MAX;

    w->settings->ui_scale = val;

    /* Apply scale to the causality instance immediately. */
    ca_instance_set_scale(w->instance, val);

    /* Persist to disk. */
    sol_settings_save(w->settings);

    /* Update the label and rebuild the content to reflect the new value. */
    sw_update_scale_label(w);
    sol_ui_bump_u32(w->sig_rev);
}

/* ------------------------------------------------------------------ */
/* Content builder                                                     */
/* ------------------------------------------------------------------ */

static void sw_render_theme_tab(SolSettingsWindow *w)
{
    /* ---- Section title ---- */
    ca_text(&(Ca_TextDesc){
        .text  = "THEME",
        .style = "sw-section-title",
    });

    ca_hr(&(Ca_HrDesc){ .style = "sw-hr" });

    /* ---- Scale row ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "sw-setting-row",
    });

    ca_text(&(Ca_TextDesc){
        .text  = "Scale",
        .style = "sw-setting-label",
    });

    ca_slider(&(Ca_SliderDesc){
        .min         = SOL_SETTINGS_UI_SCALE_MIN,
        .max         = SOL_SETTINGS_UI_SCALE_MAX,
        .value       = w->settings->ui_scale,
        .width       = SW_SCALE_SLIDER_W,
        .on_change   = sw_on_scale_change,
        .change_data = w,
        .style       = "sw-slider",
    });

    ca_text(&(Ca_TextDesc){
        .text  = w->scale_label,
        .style = "sw-setting-value",
    });

    ca_div_end(); /* sw-setting-row */
}

static void sw_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolSettingsWindow *w = (SolSettingsWindow *)user_data;

    /* Subscribe so this builder re-runs on any tab or value change. */
    (void)ca_signal_get_u32(w->sig_rev);

    /* ---- Left panel: vertical tab list ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "sw-left",
    });

    for (int i = 0; i < SW_TAB_COUNT; i++) {
        const bool active = (w->active_tab == i);
        ca_btn_begin(&(Ca_BtnDesc){
            .direction  = CA_HORIZONTAL,
            .style      = active ? "sw-tab-btn sw-tab-btn-active"
                                 : "sw-tab-btn",
            .on_click   = sw_on_tab_click,
            .click_data = &w->tab_ctxs[i],
        });
        ca_text(&(Ca_TextDesc){
            .text  = SW_TAB_LABELS[i],
            .style = active ? "sw-tab-label sw-tab-label-active"
                            : "sw-tab-label",
        });
        ca_btn_end();
    }

    ca_div_end(); /* sw-left */

    /* ---- Right panel: settings content for the active tab ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "sw-right",
    });

    switch (w->active_tab) {
        case SW_TAB_THEME: sw_render_theme_tab(w); break;
        default: break;
    }

    ca_div_end(); /* sw-right */
}

/* ------------------------------------------------------------------ */
/* Window layout                                                       */
/* ------------------------------------------------------------------ */

static void sw_build_layout(SolSettingsWindow *w)
{
    ca_ui_begin(w->window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "sw-root",
    });

    /* The body is a reactive host: left + right panels are built by the
     * content builder and re-run whenever sig_rev changes. */
    w->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "sw-body",
    });
    ca_div_set_builder(w->content_host, sw_content_builder, w);
    ca_div_end(); /* sw-body */

    ca_ui_end();
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

static void sw_destroy(SolSettingsWindow *w)
{
    if (!w) return;
    if (w->window && ca_window_is_open(w->window))
        ca_window_close(w->window);
    /* sig_rev is owned by the causality instance — not freed here. */
    free(w);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void sol_ui_settings_window_open(Ca_Instance *instance, SolSettings *settings)
{
    if (!instance || !settings) return;

    /* If a window is already open, don't open a second one. */
    for (SolSettingsWindow *w = g_sw_windows; w; w = w->next) {
        if (w->window && ca_window_is_open(w->window))
            return;
    }

    SolSettingsWindow *w = (SolSettingsWindow *)calloc(1, sizeof(SolSettingsWindow));
    if (!w) return;

    w->instance   = instance;
    w->settings   = settings;
    w->active_tab = SW_TAB_THEME;

    /* Pre-fill the scale label before the first build. */
    sw_update_scale_label(w);

    /* Populate tab context pool. */
    for (int i = 0; i < SW_TAB_COUNT; i++) {
        w->tab_ctxs[i].win       = w;
        w->tab_ctxs[i].tab_index = i;
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

void sol_ui_settings_window_tick(void)
{
    SolSettingsWindow **link = &g_sw_windows;
    while (*link) {
        SolSettingsWindow *w = *link;
        if (!w->window || !ca_window_is_open(w->window)) {
            *link     = w->next;
            w->window = NULL;   /* already closed — don't close again */
            sw_destroy(w);
            continue;
        }
        link = &w->next;
    }
}
