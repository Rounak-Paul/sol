// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* plugin_window.c — Plugin Manager window.
 *
 * Follows the same pattern as file_picker.c: each open instance owns a
 * Ca_Window created via ca_window_create().  sol_ui_plugin_window_open()
 * creates the window (or no-ops if one is already open).
 * sol_ui_plugin_window_tick() — called from sol_ui_system_tick() each
 * frame — destroys the struct when the window has been closed.
 *
 * Layout (two-panel, 840 × 520):
 *
 *   ┌── header (title) ─────────────────────────────────────────────┐
 *   ├── body ───────────────────────────────────────────────────────┤
 *   │  ┌─ left 260px ───┐  ┌─ right (flex-grow) ──────────────────┐│
 *   │  │  [search…]     │  │  Name                                ││
 *   │  │  ─────────     │  │  [Enabled] [Dynamic]                 ││
 *   │  │  Plugin A      │  │  [Disable] [Reload]                  ││
 *   │  │  Plugin B      │  │  ─────────────────                   ││
 *   │  └────────────────┘  └──────────────────────────────────────┘│
 *   └───────────────────────────────────────────────────────────────┘
 */

#include "sol_ui_internal.h"
#include "sol_plugin.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                             */
/* ------------------------------------------------------------------ */

#define PM_DEFAULT_WIDTH   840
#define PM_DEFAULT_HEIGHT  520
#define PM_LIST_MAX        64u

#define PM_LABEL_DISABLE CA_ICON_NF_FA_TOGGLE_OFF " Disable"
#define PM_LABEL_ENABLE  CA_ICON_NF_FA_TOGGLE_ON " Enable"
#define PM_LABEL_RELOAD  CA_ICON_NF_COD_DEBUG_RESTART " Reload"

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolPMItemCtx {
    struct SolPluginWindow *win;
    char                    plugin_id[128];
} SolPMItemCtx;

typedef struct SolPluginWindow {
    Ca_Window        *window;
    SolPluginManager *plugin_manager;

    /* Reactive signals owned by this window instance. */
    Ca_Signal        *sig_list_rev;      /* u32  — bumped on enable/disable/reload */
    Ca_Signal        *sig_selected_rev;  /* u32  — bumped on selection change      */

    /* Content host — set during build_layout; reactive builder
       re-runs on signal change. */
    Ca_Div           *content_host;

    /* Runtime state. */
    char              search[128];
    char              selected_id[128];

    /* Stable per-row click context pool. */
    SolPMItemCtx      item_ctxs[PM_LIST_MAX];

    /* Intrusive list link. */
    struct SolPluginWindow *next;
} SolPluginWindow;

/* ------------------------------------------------------------------ */
/* Global registry                                                     */
/* ------------------------------------------------------------------ */

static SolPluginWindow *g_pm_windows = NULL;

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static bool pm_contains_nocase(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;

    const size_t nl = strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        size_t j;
        for (j = 0; j < nl; j++) {
            if (!haystack[i + j]) break;
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nl) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                    */
/* ------------------------------------------------------------------ */

static void pm_on_item_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolPMItemCtx    *ctx = user_data;
    SolPluginWindow *w   = ctx->win;

    strncpy(w->selected_id, ctx->plugin_id, sizeof(w->selected_id) - 1u);
    w->selected_id[sizeof(w->selected_id) - 1u] = '\0';

    sol_ui_bump_u32(w->sig_selected_rev);
}

static void pm_on_disable_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolPluginWindow *w = user_data;
    if (!w->plugin_manager || !w->selected_id[0]) return;

    sol_plugin_manager_disable(w->plugin_manager, w->selected_id);
    sol_ui_bump_u32(w->sig_list_rev);
    sol_ui_bump_u32(w->sig_selected_rev);
}

static void pm_on_enable_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolPluginWindow *w = user_data;
    if (!w->plugin_manager || !w->selected_id[0]) return;

    sol_plugin_manager_enable(w->plugin_manager, w->selected_id);
    sol_ui_bump_u32(w->sig_list_rev);
    sol_ui_bump_u32(w->sig_selected_rev);
}

static void pm_on_reload_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolPluginWindow *w = user_data;
    if (!w->plugin_manager || !w->selected_id[0]) return;

    sol_plugin_manager_reload(w->plugin_manager, w->selected_id);
    sol_ui_bump_u32(w->sig_list_rev);
    sol_ui_bump_u32(w->sig_selected_rev);
}

static void pm_on_search_change(Ca_TextInput *inp, void *user_data)
{
    SolPluginWindow *w   = user_data;
    const char      *txt = ca_get_text(inp);
    size_t           n   = txt ? strlen(txt) : 0u;

    if (n >= sizeof(w->search)) n = sizeof(w->search) - 1u;
    memcpy(w->search, txt ? txt : "", n);
    w->search[n] = '\0';

    sol_ui_bump_u32(w->sig_list_rev);
}

/* ------------------------------------------------------------------ */
/* Content builder                                                     */
/* ------------------------------------------------------------------ */

static void pm_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolPluginWindow *w = user_data;

    /* Subscribe so the builder re-runs when list or selection changes. */
    (void)ca_signal_get_u32(w->sig_list_rev);
    (void)ca_signal_get_u32(w->sig_selected_rev);

    /* ---- Left panel ------------------------------------------------ */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "pm-left",
    });

    /* Search row */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "pm-search-row",
    });
    ca_input(&(Ca_InputDesc){
        .text        = w->search,
        .placeholder = "Search plugins\xe2\x80\xa6",
        .width       = 240.0f,
        .height      = 26.0f,
        .on_change   = pm_on_search_change,
        .change_data = w,
        .style       = "pm-search-input",
    });
    ca_div_end(); /* pm-search-row */

    /* Scrollable plugin list */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "pm-list",
    });

    if (w->plugin_manager) {
        const size_t count = sol_plugin_manager_count(w->plugin_manager);
        size_t       slot  = 0u;
        bool         any   = false;

        for (size_t i = 0u; i < count; i++) {
            SolPluginInfo info;
            if (!sol_plugin_manager_get_info_at(w->plugin_manager, i, &info))
                continue;

            if (w->search[0]) {
                if (!pm_contains_nocase(info.display_name, w->search) &&
                    !pm_contains_nocase(info.id,           w->search))
                    continue;
            }

            any = true;

            SolPMItemCtx *ctx = NULL;
            if (slot < PM_LIST_MAX) {
                ctx           = &w->item_ctxs[slot];
                ctx->win      = w;
                strncpy(ctx->plugin_id, info.id, sizeof(ctx->plugin_id) - 1u);
                ctx->plugin_id[sizeof(ctx->plugin_id) - 1u] = '\0';
                slot++;
            }

            const bool selected =
                w->selected_id[0] &&
                strcmp(w->selected_id, info.id) == 0;

            ca_btn_begin(&(Ca_BtnDesc){
                .direction  = CA_VERTICAL,
                .style      = selected
                                  ? "pm-item pm-item-selected"
                                  : "pm-item",
                .on_click   = ctx ? pm_on_item_click : NULL,
                .click_data = ctx,
                .id         = info.id,
            });
            ca_text(&(Ca_TextDesc){
                .text  = info.display_name,
                .style = info.enabled
                             ? "pm-item-name"
                             : "pm-item-disabled-name",
            });
            ca_text(&(Ca_TextDesc){
                .text  = info.version ? info.version : "0.0.0",
                .style = info.enabled
                             ? "pm-item-version"
                             : "pm-item-disabled-version",
            });
            ca_btn_end();
        }

        if (!any) {
            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_VERTICAL,
                .style     = "pm-empty",
            });
            ca_text(&(Ca_TextDesc){
                .text  = count == 0u
                             ? "No plugins installed."
                             : "No plugins match the search.",
                .style = "pm-empty-text",
            });
            ca_div_end();
        }
    }

    ca_div_end(); /* pm-list */
    ca_div_end(); /* pm-left */

    /* ---- Right panel ----------------------------------------------- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "pm-right",
    });

    /* Find the selected plugin. */
    SolPluginInfo sel;
    bool          found = false;

    if (w->selected_id[0] && w->plugin_manager) {
        const size_t count = sol_plugin_manager_count(w->plugin_manager);
        for (size_t i = 0u; i < count; i++) {
            SolPluginInfo tmp;
            if (!sol_plugin_manager_get_info_at(w->plugin_manager, i, &tmp))
                continue;
            if (strcmp(tmp.id, w->selected_id) == 0) {
                sel   = tmp;
                found = true;
                break;
            }
        }
    }

    if (found) {
        ca_text(&(Ca_TextDesc){
            .text  = sel.display_name,
            .style = "pm-detail-name",
        });

        /* Status badges */
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "pm-badge-row",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = sel.enabled
                             ? "pm-badge pm-badge-enabled"
                             : "pm-badge pm-badge-disabled",
        });
        ca_text(&(Ca_TextDesc){
            .text  = sel.enabled ? "Enabled" : "Disabled",
            .style = sel.enabled
                         ? "pm-badge-text pm-badge-text-enabled"
                         : "pm-badge-text pm-badge-text-disabled",
        });
        ca_div_end();

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = sel.is_dynamic
                             ? "pm-badge pm-badge-dynamic"
                             : "pm-badge pm-badge-static",
        });
        ca_text(&(Ca_TextDesc){
            .text  = sel.is_dynamic ? "Dynamic" : "Built-in",
            .style = sel.is_dynamic
                         ? "pm-badge-text pm-badge-text-dynamic"
                         : "pm-badge-text pm-badge-text-static",
        });
        ca_div_end();

        ca_div_end(); /* pm-badge-row */

        /* Action buttons */
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "pm-action-row",
        });

        if (sel.enabled) {
            ca_btn_begin(&(Ca_BtnDesc){
                .style      = "pm-btn pm-btn-disable",
                .on_click   = pm_on_disable_click,
                .click_data = w,
            });
            ca_text(&(Ca_TextDesc){
                .text  = PM_LABEL_DISABLE,
                .style = "pm-btn-text pm-btn-disable-text",
            });
            ca_btn_end();
        } else {
            ca_btn_begin(&(Ca_BtnDesc){
                .style      = "pm-btn pm-btn-enable",
                .on_click   = pm_on_enable_click,
                .click_data = w,
            });
            ca_text(&(Ca_TextDesc){
                .text  = PM_LABEL_ENABLE,
                .style = "pm-btn-text pm-btn-enable-text",
            });
            ca_btn_end();
        }

        if (sel.is_dynamic) {
            ca_btn_begin(&(Ca_BtnDesc){
                .style      = "pm-btn",
                .on_click   = pm_on_reload_click,
                .click_data = w,
                .disabled   = !sel.enabled,
            });
            ca_text(&(Ca_TextDesc){
                .text  = PM_LABEL_RELOAD,
                .style = "pm-btn-text",
            });
            ca_btn_end();
        }

        ca_div_end(); /* pm-action-row */

        ca_hr(&(Ca_HrDesc){ .style = "pm-hr" });

        /* Metadata rows */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "pm-info-row" });
        ca_text(&(Ca_TextDesc){ .text = "ID",   .style = "pm-info-key" });
        ca_text(&(Ca_TextDesc){ .text = sel.id, .style = "pm-info-value" });
        ca_div_end();

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "pm-info-row" });
        ca_text(&(Ca_TextDesc){ .text = "Version", .style = "pm-info-key" });
        ca_text(&(Ca_TextDesc){ .text = sel.version ? sel.version : "0.0.0",
                                .style = "pm-info-value" });
        ca_div_end();

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "pm-info-row" });
        ca_text(&(Ca_TextDesc){ .text = "Type", .style = "pm-info-key" });
        ca_text(&(Ca_TextDesc){ .text = sel.is_dynamic ? "Dynamic" : "Built-in",
                                .style = "pm-info-value" });
        ca_div_end();

        if (sel.is_dynamic && sel.path) {
            ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "pm-info-row" });
            ca_text(&(Ca_TextDesc){ .text = "Path",   .style = "pm-info-key" });
            ca_text(&(Ca_TextDesc){ .text = sel.path, .style = "pm-info-value" });
            ca_div_end();
        }

    } else {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "pm-empty",
        });
        ca_text(&(Ca_TextDesc){
            .text  = "Select a plugin from the list.",
            .style = "pm-empty-text",
        });
        ca_div_end();
    }

    ca_div_end(); /* pm-right */
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void pm_build_layout(SolPluginWindow *w)
{
    ca_ui_begin(w->window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "pm-root",
    });

    w->content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "pm-body",
    });
    ca_div_set_builder(w->content_host, pm_content_builder, w);
    ca_div_end(); /* pm-body */

    ca_ui_end();
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

static void pm_destroy(SolPluginWindow *w)
{
    if (!w) return;
    if (w->window && ca_window_is_open(w->window))
        ca_window_close(w->window);
    /* Signals are owned by the causality instance — not freed here. */
    free(w);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void sol_ui_plugin_window_open(Ca_Instance *instance, SolPluginManager *pm)
{
    if (!instance) return;

    /* If a window is already open, don't open a second one. */
    for (SolPluginWindow *w = g_pm_windows; w; w = w->next) {
        if (w->window && ca_window_is_open(w->window))
            return;
    }

    SolPluginWindow *w = (SolPluginWindow *)calloc(1u, sizeof(SolPluginWindow));
    if (!w) return;

    w->plugin_manager = pm;

    w->sig_list_rev     = ca_signal_u32(instance, 0u);
    w->sig_selected_rev = ca_signal_u32(instance, 0u);
    if (!w->sig_list_rev || !w->sig_selected_rev) {
        free(w);
        return;
    }

    w->window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = "Plugin Manager",
        .width  = PM_DEFAULT_WIDTH,
        .height = PM_DEFAULT_HEIGHT,
    });
    if (!w->window) {
        free(w);
        return;
    }

    pm_build_layout(w);

    w->next      = g_pm_windows;
    g_pm_windows = w;
}

void sol_ui_plugin_window_tick(void)
{
    SolPluginWindow **link = &g_pm_windows;
    while (*link) {
        SolPluginWindow *w = *link;
        if (!w->window || !ca_window_is_open(w->window)) {
            *link     = w->next;
            w->window = NULL;   /* already closed — don't try to close again */
            pm_destroy(w);
            continue;
        }
        link = &w->next;
    }
}
