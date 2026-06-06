// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* workspace.c — Sol's UI lifecycle, workspace-tree rendering, top-level
 * layout, input event routing, and the built-in command-flow actions.
 *
 * Layout structure (causality-managed strips wrapped around content_root):
 *   <causality title bar>            (system-managed)
 *   content_root
 *   └── app-root  (this is what ca_ui_begin attaches to)
 *       └── workspace-host
 *           ├── workspace-content-host  (reactive; workspace builder)
 *           │   └── workspace-main-content (file tree + buffer area)
 *           └── popup-host              (reactive; absolute overlay)
 *               └── cf-panel            (only when leader_active)
 *   <causality status bar>           (system-managed; sol installs builder)
 *
 * Reactive design (idiomatic causality):
 *
 *   State IS the signal. Each coherent piece of UI-driving state owns
 *   a Ca_Signal*. Mutations write the signal; builders subscribe by
 *   reading it inside their body via ca_signal_get_*. The runtime
 *   re-runs the affected builders — nothing else.
 *
 *   Data-layer signals self-notify:
 *     - SolBufferSystem owns sig_buffer_rev; every sol_buffer_*
 *       mutator bumps it. The workspace content builder subscribes.
 *     - SolFileTree owns sig_file_tree_rev; every sol_file_tree_*
 *       mutator bumps it. The tree panel reads it.
 *
 *   UI-only signals (this file owns):
 *     - sig_leader_active (bool)
 *     - sig_file_tree_visible (bool)
 *     - sig_leader_prefix_rev (u32)
 *     - sig_flow_registry_rev (u32)
 *     - sig_window_rev (u32)
 *
 *   There are no "invalidate" helpers, no manual ca_div_invalidate, no
 *   deferred dirty flag, and no on-frame dirty drain — the framework
 *   owns scheduling.
 */

#include "sol_ui_internal.h"

#include "sol_file_picker.h"
#include "sol_event.h"
#include "style.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations for title-bar trampolines (Ca_MenuActionFn) */
static void sol_ui_menu_new_buffer_action(void *user_data);
static void sol_ui_menu_open_file_action(void *user_data);
static void sol_ui_menu_open_folder_action(void *user_data);
static void sol_ui_menu_open_plugin_manager_action(void *user_data);
static void sol_ui_menu_open_settings_action(void *user_data);
/* Forward declarations for welcome-screen button clicks (Ca_ClickFn) */
static void sol_ui_welcome_click_new_buffer(Ca_Button *btn, void *user_data);
static void sol_ui_welcome_click_open_file(Ca_Button *btn, void *user_data);
static void sol_ui_welcome_click_open_folder(Ca_Button *btn, void *user_data);

/* ------------------------------------------------------------------ */
/* Internal context types                                              */
/* ------------------------------------------------------------------ */

typedef struct SolWorkspaceVisitorContext {
    SolUISystem *ui;
} SolWorkspaceVisitorContext;

/* Causality places its custom title bar at a fixed 30 px height (see
   causality/src/ui/title_bar.c). */
#define SOL_UI_TITLE_BAR_HEIGHT_PX  30
/* Buffer split-tree root geometry.
   The tree lives below the global tab strip and inside the right pane of
   the workspace split. */
#define SOL_UI_BUFFER_TAB_STRIP_HEIGHT_PX 28.0f
#define SOL_UI_BUFFER_SPLIT_BAR_SIZE_PX    1.0f

static bool sol_ui_buffer_area_rect_internal(const SolUISystem *ui,
                                             float *out_x,
                                             float *out_y,
                                             float *out_w,
                                             float *out_h)
{
    if (!ui || ui->window_w <= 0 || ui->window_h <= 0) {
        return false;
    }

    const float title_h  = (float)SOL_UI_TITLE_BAR_HEIGHT_PX;
    const float status_h = (float)SOL_UI_STATUS_BAR_HEIGHT;
    const float tabs_h   = SOL_UI_BUFFER_TAB_STRIP_HEIGHT_PX;

    float root_x = 0.0f;
    float root_y = title_h + tabs_h;
    float root_w = (float)ui->window_w;
    float root_h = (float)ui->window_h - title_h - status_h - tabs_h;

    if (root_h < 0.0f) root_h = 0.0f;

    const bool has_tree_root =
        (ui->file_tree &&
         sol_ui_system_file_tree_visible(ui) &&
         sol_file_tree_root(ui->file_tree) != NULL);
    if (has_tree_root) {
        float avail_w = root_w - SOL_UI_BUFFER_SPLIT_BAR_SIZE_PX;
        if (avail_w < 0.0f) avail_w = 0.0f;
        const float ratio = ui->tree_panel_ratio < 0.0f ? 0.0f
                             : (ui->tree_panel_ratio > 1.0f ? 1.0f
                             : ui->tree_panel_ratio);
        float tree_w = avail_w * ratio;
        if (tree_w < 0.0f) tree_w = 0.0f;
        root_x += tree_w + SOL_UI_BUFFER_SPLIT_BAR_SIZE_PX;
        root_w = avail_w - tree_w;
        if (root_w < 0.0f) root_w = 0.0f;
    }

    if (out_x) *out_x = root_x;
    if (out_y) *out_y = root_y;
    if (out_w) *out_w = root_w;
    if (out_h) *out_h = root_h;
    return true;
}

/* ------------------------------------------------------------------ */
/* Reactive helpers                                                    */
/* ------------------------------------------------------------------ */

/* Bump a u32 revision signal: read current value (no subscription —
   this runs outside any effect context), then write +1. Notifies
   every effect that subscribed via ca_signal_get_u32 during its last
   evaluation. */
void sol_ui_bump_u32(Ca_Signal *sig)
{
    if (!sig) {
        return;
    }
    ca_signal_set_u32(sig, ca_signal_get_u32(sig) + 1u);
}

void sol_ui_system_set_file_tree_visible(SolUISystem *ui, bool visible)
{
    if (!ui) return;
    if (ui->file_tree_visible == visible) return;
    ui->file_tree_visible = visible;
    if (ui->sig_file_tree_visible) {
        ca_signal_set_bool(ui->sig_file_tree_visible, visible);
    }
}

bool sol_ui_system_file_tree_visible(const SolUISystem *ui)
{
    return ui ? ui->file_tree_visible : false;
}

const char *sol_ui_system_file_tree_root(const SolUISystem *ui)
{
    return (ui && ui->file_tree) ? sol_file_tree_root(ui->file_tree) : NULL;
}


/* ------------------------------------------------------------------ */
/* Buffer workspace visitor                                            */
/* ------------------------------------------------------------------ */

static int sol_ui_split_direction_to_ca(SolBufferSplitDirection direction)
{
    return direction == SOL_BUFFER_SPLIT_VERTICAL ? CA_HORIZONTAL : CA_VERTICAL;
}

static void sol_ui_split_on_resize(float ratio, void *user_data)
{
    SolSplitCallbackCtx *ctx = (SolSplitCallbackCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->buffers || ctx->node_id == 0u) {
        return;
    }
    sol_buffer_set_split_ratio(ctx->ui->buffers, ctx->node_id, ratio);
}

static void sol_ui_visit_begin_split(SolBufferSplitDirection direction,
                                     float ratio, SolBufferNodeId node_id,
                                     void *user_data)
{
    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }

    SolSplitCallbackCtx *cb_ctx = NULL;
    if (ctx->ui->split_callback_ctx_count < SOL_UI_MAX_SPLIT_CALLBACKS) {
        cb_ctx = &ctx->ui->split_callback_ctxs[ctx->ui->split_callback_ctx_count++];
        cb_ctx->ui = ctx->ui;
        cb_ctx->node_id = node_id;
    } else {
        /* Pool exhausted: this split's drag will not persist across
           rebuilds. The ceiling is generous; if you hit this, raise
           SOL_UI_MAX_SPLIT_CALLBACKS. */
        assert(false && "SOL_UI_MAX_SPLIT_CALLBACKS exceeded");
    }

    ca_split_begin(&(Ca_SplitDesc){
        .direction       = sol_ui_split_direction_to_ca(direction),
        .ratio           = ratio,
        .bar_size        = SOL_UI_SPLIT_BAR_SIZE,
        .bar_color       = SOL_UI_SPLIT_BAR_COLOR,
        .bar_hover_color = SOL_UI_SPLIT_BAR_HOVER_COLOR,
        .on_resize       = cb_ctx ? sol_ui_split_on_resize : NULL,
        .user_data       = cb_ctx,
    });
}

static void sol_ui_visit_end_split(void *user_data)
{
    (void)user_data;
    ca_split_end();
}

/* ------------------------------------------------------------------ */
/* Pane-click context pool                                             */
/* ------------------------------------------------------------------ */

static SolPaneClickCtx *sol_ui_acquire_pane_click_ctx(SolUISystem *ui)
{
    if (!ui) return NULL;
    if (ui->pane_click_ctx_count == ui->pane_click_ctx_capacity) {
        size_t new_cap = ui->pane_click_ctx_capacity
                            ? ui->pane_click_ctx_capacity * 2u
                            : 32u;
        SolPaneClickCtx *grown = (SolPaneClickCtx *)realloc(
            ui->pane_click_ctxs, new_cap * sizeof(SolPaneClickCtx));
        if (!grown) return NULL;
        ui->pane_click_ctxs        = grown;
        ui->pane_click_ctx_capacity = new_cap;
    }
    return &ui->pane_click_ctxs[ui->pane_click_ctx_count++];
}

static void sol_ui_on_pane_click(Ca_Button *button, void *user_data)
{
    (void)button;
    SolPaneClickCtx *cb = (SolPaneClickCtx *)user_data;
    if (!cb || !cb->ui || !cb->ui->buffers) return;
    if (cb->ui->focus_region_callback) {
        cb->ui->focus_region_callback(false, cb->ui->focus_region_user_data);
    }
    /* sol_buffer_set_active_leaf self-notifies via sig_buffer_rev when
       the leaf actually changes — no explicit invalidation needed. */
    (void)sol_buffer_set_active_leaf(cb->ui->buffers, cb->leaf_id);
}

static void sol_ui_on_tab_click(Ca_Button *button, void *user_data)
{
    (void)button;
    SolPaneClickCtx *cb = (SolPaneClickCtx *)user_data;
    if (!cb || !cb->ui || !cb->ui->buffers) return;
    if (cb->ui->focus_region_callback) {
        cb->ui->focus_region_callback(false, cb->ui->focus_region_user_data);
    }
    /* Both sol_buffer_set_active_leaf and sol_buffer_set_leaf_buffer
       self-notify on success. */
    (void)sol_buffer_set_active_leaf(cb->ui->buffers, cb->leaf_id);
    if (cb->tab_buffer_id != 0u) {
        (void)sol_buffer_set_leaf_buffer(cb->ui->buffers, cb->leaf_id,
                                         cb->tab_buffer_id);
    }
}

static void sol_ui_on_tab_close(Ca_Button *button, void *user_data)
{
    (void)button;
    SolPaneClickCtx *cb = (SolPaneClickCtx *)user_data;
    if (!cb || !cb->ui || !cb->ui->buffers) return;
    if (cb->tab_buffer_id != 0u) {
        sol_buffer_close(cb->ui->buffers, cb->tab_buffer_id);
    }
}

/* Global tab strip rendered ONCE above the entire split tree (Neovim
   tabline style). Each tab is one live buffer; clicking a tab focuses
   the currently-active leaf and swaps in that buffer. The active tab
   is the one whose buffer the active leaf is currently showing. */
static void sol_ui_render_global_tab_strip(SolUISystem *ui)
{
    if (!ui || !ui->buffers) return;
    const size_t tab_count = sol_buffer_count(ui->buffers);
    if (tab_count == 0u) return;

    const SolBufferNodeId active_leaf  = sol_buffer_active_leaf(ui->buffers);
    const SolBufferId     active_bufid = sol_buffer_leaf_buffer(ui->buffers, active_leaf);

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "buffer-tabs-row",
    });
    for (size_t i = 0u; i < tab_count; ++i) {
        const SolBufferId tab_id = sol_buffer_at(ui->buffers, i);
        if (tab_id == 0u) continue;
        const SolBuffer *tab_buf = sol_buffer_get_const(ui->buffers, tab_id);
        if (!tab_buf) continue;
        const bool tab_active = (tab_id == active_bufid);
        SolPaneClickCtx *cb = sol_ui_acquire_pane_click_ctx(ui);
        if (cb) {
            cb->ui            = ui;
            cb->leaf_id       = active_leaf;
            cb->tab_buffer_id = tab_id;
        }
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = tab_active ? "buffer-tab buffer-tab-active"
                                      : "buffer-tab",
            .direction  = CA_HORIZONTAL,
            .background = 0u,
            .on_click   = cb ? sol_ui_on_tab_click : NULL,
            .click_data = cb,
        });
        ca_text(&(Ca_TextDesc){
            .text  = sol_buffer_name(tab_buf),
            .style = tab_active ? "buffer-tab-text buffer-tab-text-active"
                                : "buffer-tab-text",
        });
        /* Close button — separate ctx so its click doesn't fire the tab switch. */
        SolPaneClickCtx *close_cb = sol_ui_acquire_pane_click_ctx(ui);
        if (close_cb) {
            close_cb->ui            = ui;
            close_cb->leaf_id       = active_leaf;
            close_cb->tab_buffer_id = tab_id;
        }
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "buffer-tab-close",
            .direction  = CA_HORIZONTAL,
            .background = 0u,
            .on_click   = close_cb ? sol_ui_on_tab_close : NULL,
            .click_data = close_cb,
        });
        ca_text(&(Ca_TextDesc){
            .text  = "\xc3\x97",   /* U+00D7 × */
            .style = tab_active ? "buffer-tab-close-icon buffer-tab-close-icon-active"
                                : "buffer-tab-close-icon",
        });
        ca_btn_end();  /* buffer-tab-close */
        ca_btn_end();  /* buffer-tab */
    }
    ca_div_end();   /* buffer-tabs-row */
}

static void sol_ui_visit_render_leaf(SolBuffer *buffer, SolBufferNodeId leaf_id,
                                     bool is_active, const SolBufferRect *rect,
                                     void *user_data)
{
    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }
    SolUISystem *ui = ctx->ui;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = is_active ? "buffer-pane buffer-pane-active" : "buffer-pane",
    });

    /* Pane body wrapped in a button so clicking anywhere inside the
       buffer area focuses this pane (without disturbing the buffer's
       own contents). */
    {
        SolPaneClickCtx *cb = sol_ui_acquire_pane_click_ctx(ui);
        if (cb) {
            cb->ui            = ui;
            cb->leaf_id       = leaf_id;
            cb->tab_buffer_id = 0u;
        }
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "buffer-body",
            .direction  = CA_VERTICAL,
            .background = 0u,
            .on_click   = cb ? sol_ui_on_pane_click : NULL,
            .click_data = cb,
        });
    }

    if (buffer) {
        SolBufferRenderArgs args = {
            .is_active   = is_active,
            .ui_context  = ui,
            .leaf_id     = leaf_id,
        };
        if (rect) {
            args.rect = *rect;
        }
        sol_buffer_render(buffer, &args);
    }

    ca_btn_end();  /* buffer-body */
    ca_div_end();  /* buffer-pane */
}

void sol_ui_render_workspace_tree(SolUISystem *ui)
{
    if (!ui || !ui->buffers) {
        return;
    }

    if (sol_buffer_count(ui->buffers) == 0u) {
        /* Welcome screen — shown whenever no buffers are open. */
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "welcome-pane",
        });

        ca_text(&(Ca_TextDesc){ .text = "Sol Editor", .style = "welcome-title" });
        ca_text(&(Ca_TextDesc){ .text = "A fast, minimal text editor.", .style = "welcome-subtitle" });

        /* Action buttons */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-actions" });
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "New Buffer",
            .style      = "welcome-btn-primary",
            .on_click   = sol_ui_welcome_click_new_buffer,
            .click_data = ui,
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "Open File\xe2\x80\xa6",
            .style      = "welcome-btn",
            .on_click   = sol_ui_welcome_click_open_file,
            .click_data = ui,
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "Open Folder\xe2\x80\xa6",
            .style      = "welcome-btn",
            .on_click   = sol_ui_welcome_click_open_folder,
            .click_data = ui,
        });
        ca_btn_end();
        ca_div_end(); /* welcome-actions */

        ca_hr(&(Ca_HrDesc){ .style = "welcome-hr" });

        /* Two-column shortcut reference */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-cols" });

        /* Left column: BUFFER + EXPLORER */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "welcome-col" });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "welcome-section" });
        ca_text(&(Ca_TextDesc){ .text = "BUFFER", .style = "welcome-section-label" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl b c", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "New buffer", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl b o", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Open file", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl b x", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Close buffer", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl b b", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Previous buffer", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl b n / p", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Next / prev buffer", .style = "welcome-desc" });
        ca_div_end();
        ca_div_end(); /* welcome-section BUFFER */

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "welcome-section" });
        ca_text(&(Ca_TextDesc){ .text = "EXPLORER", .style = "welcome-section-label" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl e e", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Toggle panel", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl e o", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Open folder", .style = "welcome-desc" });
        ca_div_end();
        ca_div_end(); /* welcome-section EXPLORER */

        ca_div_end(); /* welcome-col left */

        /* Right column: PANE */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "welcome-col" });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "welcome-section" });
        ca_text(&(Ca_TextDesc){ .text = "PANE", .style = "welcome-section-label" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl p v", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Split vertical", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl p h", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Split horizontal", .style = "welcome-desc" });
        ca_div_end();
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-row" });
        ca_text(&(Ca_TextDesc){ .text = "ctrl p n / p", .style = "welcome-key" });
        ca_text(&(Ca_TextDesc){ .text = "Focus next / prev pane", .style = "welcome-desc" });
        ca_div_end();
        ca_div_end(); /* welcome-section PANE */

        ca_div_end(); /* welcome-col right */

        ca_div_end(); /* welcome-cols */

        ca_div_end(); /* welcome-pane */
        return;
    }

    SolWorkspaceVisitorContext visitor_context = { .ui = ui };

    /* Reset the per-frame split callback pool. Pointers handed out
       below stay valid until the next call to this function. */
    ui->split_callback_ctx_count = 0u;
    /* Note: pane_click_ctx pool is reset by the buffer-area builder so
       it spans both the global tab strip and the split-tree. */

    SolBufferWorkspaceVisitor visitor;
    memset(&visitor, 0, sizeof(visitor));
    visitor.begin_split  = sol_ui_visit_begin_split;
    visitor.end_split    = sol_ui_visit_end_split;
    visitor.render_leaf  = sol_ui_visit_render_leaf;

    float root_x = 0.0f, root_y = 0.0f, root_w = 0.0f, root_h = 0.0f;
    if (!sol_ui_buffer_area_rect_internal(ui, &root_x, &root_y, &root_w, &root_h)) {
        return;
    }
    SolBufferRect root_rect = {
        .x = root_x,
        .y = root_y,
        .w = root_w,
        .h = root_h,
    };
    sol_buffer_workspace_visit(ui->buffers, &root_rect, &visitor, &visitor_context);
}

static void sol_ui_on_panel_resize(float ratio, void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui) ui->tree_panel_ratio = ratio;
}

/* ------------------------------------------------------------------ */
/* Reactive content builder                                            */
/* ------------------------------------------------------------------ */

static void sol_ui_workspace_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }

    /* Subscribe this effect to every signal whose state this builder
       reads. Causality re-runs us exactly when one of them changes:
         - sig_buffer_rev    : the buffer/split tree (auto-bumped by
                               sol_buffer_*)
         - sig_file_tree_rev : the file tree contents (auto-bumped by
                               sol_file_tree_*)
         - sig_file_tree_visible : explorer panel visibility
         - sig_window_rev    : window resize — layout-sensitive
                               children (split bars, tabs) re-flow. */
    (void)ca_signal_get_u32(ui->sig_buffer_rev);
    (void)ca_signal_get_u32(ui->sig_file_tree_rev);
    (void)ca_signal_get_bool(ui->sig_file_tree_visible);
    (void)ca_signal_get_u32(ui->sig_window_rev);

    /* Top region: optional left tree panel + buffer area.
       When the tree is visible we use ca_split_begin so the divider is
       user-draggable; when hidden we render the buffer area directly. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "workspace-main-content",
    });

    const bool has_tree_root =
        (ui->file_tree &&
         sol_ui_system_file_tree_visible(ui) &&
         sol_file_tree_root(ui->file_tree) != NULL);

    if (has_tree_root) {
        ca_split_begin(&(Ca_SplitDesc){
            .direction      = CA_HORIZONTAL,
            .ratio          = ui->tree_panel_ratio,
            .min_ratio      = 0.10f,
            .max_ratio      = 0.50f,
            .bar_size       = 1.0f,
            .bar_color       = 0x181e2eff,
            .bar_hover_color = 0x2d3a5aff,
            .on_resize      = sol_ui_on_panel_resize,
            .user_data      = ui,
        });

        /* Left pane — file tree */
        ui->tree_panel_host = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "tree-panel",
        });
        sol_ui_render_file_tree_panel_body(ui);
        ca_div_end();   /* tree-panel (left pane) */

        /* Right pane — buffer area */
        ui->buffer_area_host = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "workspace-buffer-area",
        });
        ui->pane_click_ctx_count = 0u;
        sol_ui_render_global_tab_strip(ui);
        sol_ui_render_workspace_tree(ui);
        ca_div_end();   /* workspace-buffer-area (right pane) */

        ca_split_end();
    } else {
        ui->tree_panel_host = NULL;

        ui->buffer_area_host = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "workspace-buffer-area",
        });
        ui->pane_click_ctx_count = 0u;
        sol_ui_render_global_tab_strip(ui);
        sol_ui_render_workspace_tree(ui);
        ca_div_end();   /* workspace-buffer-area */
    }

    ca_div_end();   /* workspace-main-content */

    /* Command-flow popup is rendered by its own reactive host
       (ui->popup_host, mounted as a sibling of workspace_content_host).
       Toggling the popup invalidates only that host — the workspace
       tree (file tree + buffer split + tabs) is untouched. */
}

/* Builder installed on the popup host. The host is an absolute-positioned
   overlay covering workspace_host; while the popup is inactive the builder
   emits no children, so the host is an inert transparent rectangle —
   causality only dispatches input to buttons / focusables, so an empty
   absolute div with no background and no handlers blocks nothing. */
static void sol_ui_popup_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }
    /* Subscribe to the leader-active signal. Open/close re-runs us. */
    const bool active = ca_signal_get_bool(ui->sig_leader_active);
    if (!active) {
        return;
    }
    /* When open we additionally depend on the leader-prefix and
       flow-registry revisions: prefix advance or a new flow
       registration changes the suggestion set. We do NOT subscribe
       to these when closed — typing while no popup is open must not
       force the popup host to re-evaluate. */
    (void)ca_signal_get_u32(ui->sig_leader_prefix_rev);
    (void)ca_signal_get_u32(ui->sig_flow_registry_rev);
    sol_ui_render_command_flow_panel(ui);
}

/* Builder installed into the system-managed status bar via
   ca_window_set_status_bar. Causality has already entered the bar's
   widget context, so this only emits children. */
static void sol_ui_status_bar_builder(Ca_Window *window, void *user_data)
{
    (void)window;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }
    sol_ui_render_status_bar(ui);
}

static void sol_ui_on_frame(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }
    /* Reap closed file-picker windows. Safe even when none are open.
       Reactive scheduling is owned by causality — nothing else to
       drive from here. */
    (void)ui;
    sol_file_picker_tick();
    sol_ui_settings_window_tick();
    sol_ui_search_window_tick();

    /* Drive caret blink: while a buffer is focused, bump sig_buffer_rev
     * so the workspace-content builder re-runs every tick and evaluates
     * the current blink phase.  Then wake the instance so the next tick
     * fires even from glfwWaitEvents mode.
     *
     * on_frame is called at ctx depth=-1 (between layout and paint),
     * which is the safe window for triggering a reactive flush via
     * sol_ui_bump_u32 without overflowing the widget stack. */
    if (ui->buffers && sol_buffer_active_buffer(ui->buffers) != 0) {
        sol_ui_bump_u32(ui->sig_buffer_rev);
        ca_instance_wake();
    }
}

void sol_ui_system_pre_tick(SolUISystem *ui)
{
    if (!ui || !ui->primary_window || !ui->sig_tree_scroll) return;
    /* Poll the tree scroll container's current offset and update the signal.
       ca_signal_set_float is a no-op when the value is unchanged (memcmp),
       so the sticky-builder effect only re-runs on frames where the scroll
       actually moved.  Called before ca_instance_tick so the effect fires
       within the same frame's reactive flush. */
    float sy = ca_get_scroll_y(ui->primary_window, "tree-list");
    ca_signal_set_float(ui->sig_tree_scroll, sy);

    /* Keep the sticky overlay's width clamped to the tree panel's actual
       laid-out pixel width.  tree_panel_host->w is the computed value from
       the previous frame's layout pass — accurate enough for a one-frame
       lag that is never visible. */
    if (ui->tree_sticky_host && ui->tree_panel_host) {
        float panel_w = ca_div_get_layout_width(ui->tree_panel_host);
        /* Subtract the built-in scrollbar width (14px) so sticky rows
           don't overdraw it. */
        if (panel_w > 14.0f)
            ca_div_set_width(ui->tree_sticky_host, panel_w - 14.0f);
    }
}

static bool sol_ui_build_layout(SolUISystem *ui)
{
    if (!ui || !ui->primary_window) {
        return false;
    }

    ca_ui_begin(ui->primary_window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "app-root",
    });

    ui->workspace_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "workspace-host",
    });

    ui->workspace_content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "workspace-main-full",
    });

    /* set_builder runs the builder once synchronously on registration
       (and on every invalidate); it clears children before each run. Do
       NOT also call the builder explicitly here — that would emit two
       copies of the workspace content tree. */
    ca_div_set_builder(ui->workspace_content_host, sol_ui_workspace_content_builder, ui);

    ca_div_end();   /* workspace-content-host */

    /* Popup host — absolute overlay sibling of workspace_content_host.
       Its own reactive builder reads leader_active and emits the
       which-key card only when the popup is open. Because it lives
       outside the content host's effect, toggling the popup or
       advancing the leader prefix invalidates only this host. */
    ui->popup_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .position  = CA_POSITION_ABSOLUTE,
        .pos_x     = 0.0f,
        .pos_y     = 0.0f,
        .z_index   = 50,
        .style     = "cf-overlay",
        .no_hover  = true,   /* transparent to hover — children (popup panels) still hit-test */
    });
    ca_div_set_builder(ui->popup_host, sol_ui_popup_builder, ui);
    ca_div_end();   /* popup_host */

    /* Sticky-ancestor overlay — absolute-positioned sibling of
       workspace_content_host.  Starts just below the static section header
       (28 px) and root row (24 px), so its first row lands exactly at the
       top of the tree-scroll-area.  z_index 5 places it above the list
       content but well below the popup overlay (z 50). */
    ui->tree_sticky_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .position  = CA_POSITION_ABSOLUTE,
        .pos_x     = 0.0f,
        .pos_y     = SOL_UI_TREE_STICKY_TOP,
        .z_index   = 5,
        .style     = "tree-sticky-host",
        .no_hover  = true,   /* transparent to hover — sticky-row children still hit-test */
    });
    ca_div_set_builder(ui->tree_sticky_host, sol_ui_sticky_tree_builder, ui);
    ca_div_end();   /* tree_sticky_host */

    ca_div_end();   /* workspace-host */

    ca_ui_end();
    return true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers)
{
    if (!instance || !buffers) {
        return NULL;
    }

    SolUISystem *ui = (SolUISystem *)calloc(1u, sizeof(SolUISystem));
    if (!ui) {
        return NULL;
    }

    ui->instance        = instance;
    ui->buffers         = buffers;
    ui->leader_modifier = SOL_MOD_CTRL;
    ui->status_bar_kind = SOL_UI_STATUS_KIND_KEY;
    ui->tree_panel_ratio = 0.20f;
    ui->file_tree_visible = false;

    /* ---- Reactive state ----
       All signals are owned by the instance and freed in
       ca_instance_destroy; sol never calls ca_signal_destroy. Created
       BEFORE the window so the layout builder can safely subscribe.

       The buffer system's revision signal is created here and attached
       to ui->buffers; from this point every successful sol_buffer_*
       mutation auto-notifies our content builder.

       The file tree is created eagerly (it's a UI concern that lives
       for the UI system's lifetime) and its revision signal attached;
       the builder always reads sig_file_tree_rev so it stays
       subscribed across set_root attach/detach. */
    ui->sig_buffer_rev        = ca_signal_u32  (instance, 0u);
    ui->sig_file_tree_rev     = ca_signal_u32  (instance, 0u);
    ui->sig_file_tree_visible = ca_signal_bool(instance, false);
    ui->sig_leader_active     = ca_signal_bool (instance, false);
    ui->sig_leader_prefix_rev = ca_signal_u32  (instance, 0u);
    ui->sig_flow_registry_rev = ca_signal_u32  (instance, 0u);
    ui->sig_window_rev        = ca_signal_u32  (instance, 0u);
    ui->sig_tree_scroll       = ca_signal_float(instance, 0.0f);
    if (!ui->sig_buffer_rev || !ui->sig_file_tree_rev ||
        !ui->sig_file_tree_visible ||
        !ui->sig_leader_active || !ui->sig_leader_prefix_rev ||
        !ui->sig_flow_registry_rev || !ui->sig_window_rev ||
        !ui->sig_tree_scroll) {
        free(ui);
        return NULL;
    }
    sol_buffer_attach_revision_signal(buffers, ui->sig_buffer_rev);

    ui->file_tree = sol_file_tree_create();
    if (!ui->file_tree) {
        free(ui);
        return NULL;
    }
    sol_file_tree_attach_revision_signal(ui->file_tree, ui->sig_file_tree_rev);
    /* Share the same event bus as the buffer system so file-tree
       events fan out to the same observers without a separate hookup
       in main.c. No-op when buffers has no bus attached yet. */
    sol_file_tree_attach_event_bus(ui->file_tree, sol_buffer_event_bus(buffers));

    /* Seed the cached window size with the configured initial size so
       the first render — which happens before any resize event — has a
       sensible value to derive scroll viewport metrics from. The real
       size lands here as soon as the first resize callback fires. */
    ui->window_w = SOL_UI_WINDOW_WIDTH;
    ui->window_h = SOL_UI_WINDOW_HEIGHT;

    ui->stylesheet = ca_css_parse(SOL_UI_MAIN_WINDOW_CSS);
    if (ui->stylesheet) {
        ca_instance_set_stylesheet(instance, ui->stylesheet);
    }

    ui->primary_window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = SOL_UI_WINDOW_TITLE,
        .width  = SOL_UI_WINDOW_WIDTH,
        .height = SOL_UI_WINDOW_HEIGHT,
    });

    if (!ui->primary_window) {
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        free(ui);
        return NULL;
    }

    if (!sol_ui_build_layout(ui)) {
        ca_window_destroy(ui->primary_window);
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        free(ui);
        return NULL;
    }

    ca_window_set_on_frame(ui->primary_window, sol_ui_on_frame, ui);

    /* Hand the bottom-of-window strip over to causality so we never have
       to subtract its height from layout calculations. */
    ca_window_set_status_bar(ui->primary_window,
                             sol_ui_status_bar_builder,
                             ui,
                             SOL_UI_STATUS_BAR_HEIGHT);
    return ui;
}

void sol_ui_system_destroy(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    if (ui->file_tree) {
        sol_file_tree_destroy(ui->file_tree);
        ui->file_tree = NULL;
    }
    free(ui->file_tree_click_ctxs);
    ui->file_tree_click_ctxs = NULL;
    free(ui->pane_click_ctxs);
    ui->pane_click_ctxs = NULL;

    if (ui->primary_window) {
        ca_window_destroy(ui->primary_window);
        ui->primary_window = NULL;
    }
    if (ui->instance && ui->stylesheet) {
        ca_instance_set_stylesheet(ui->instance, NULL);
    }
    if (ui->stylesheet) {
        ca_css_destroy(ui->stylesheet);
        ui->stylesheet = NULL;
    }

    free(ui);
}

Ca_Window *sol_ui_system_primary_window(SolUISystem *ui)
{
    return ui ? ui->primary_window : NULL;
}

/* ------------------------------------------------------------------ */
/* Input event routing                                                 */
/* ------------------------------------------------------------------ */

bool sol_ui_system_handle_input_event(SolUISystem *ui, const SolInputEvent *event)
{
    if (!ui || !event || event->type != SOL_INPUT_EVENT_KEY_DOWN) {
        return false;
    }

    const SolKeyCode      key  = sol_ui_normalize_flow_key(event->data.key.key);
    const SolModifierMask mods = event->data.key.modifiers;

    /* Update status bar with the most recent keystroke. */
    if (sol_ui_is_leader_key(ui, key)) {
        char buf[48];
        sol_ui_format_modified_key(mods, key, buf, sizeof(buf));
        sol_ui_set_status_text(ui, SOL_UI_STATUS_KIND_LEADER, buf);
    } else {
        sol_ui_set_status_key(ui, key, mods);
    }

    /* Escape always closes the leader popup, but never blocks the event
       when the popup is closed (so app-level handlers can still see it). */
    if (key == SOL_KEY_ESCAPE) {
        if (ui->leader_active) {
            sol_ui_close_leader_popup(ui);
            return true;
        }
        return false;
    }

    /* Toggle popup when the leader modifier itself is pressed (debounced
       against repeats). */
    if (sol_ui_is_leader_key(ui, key)) {
        if (event->data.key.repeated) {
            return true;
        }
        if (ui->leader_active) {
            sol_ui_close_leader_popup(ui);
        } else {
            sol_ui_open_leader_popup(ui);
        }
        return true;
    }

    /* Leader-as-chord: leader+key opens flow mode and uses this key as
       the first step of the sequence. */
    if (!ui->leader_active) {
        if ((mods & ui->leader_modifier) == 0u || sol_ui_is_modifier_key(key)) {
            return false;
        }
        sol_ui_open_leader_popup(ui);
    }

    if (sol_ui_is_modifier_key(key)) {
        return true;
    }

    /* Modifier mask the user is holding for THIS step, with the leader
       modifier stripped (it's implicit while the popup is open). */
    const SolModifierMask step_mods =
        (SolModifierMask)(mods & ~ui->leader_modifier);

    /* Display the attempted sequence in the status bar before matching. */
    SolKeyCode attempted[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t attempted_len = ui->leader_prefix_length;
    if (attempted_len > SOL_UI_MAX_FLOW_SEQUENCE_LEN - 1u) {
        attempted_len = SOL_UI_MAX_FLOW_SEQUENCE_LEN - 1u;
    }
    for (size_t i = 0u; i < attempted_len; ++i) {
        attempted[i] = ui->leader_prefix[i];
    }
    attempted[attempted_len++] = key;
    /* Build matching per-step modifier array for the status bar display.
       Intermediate steps come from leader_prefix_modifiers; the last
       step uses the raw `mods` from the current event. */
    SolModifierMask attempted_step_mods[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    for (size_t i = 0u; i + 1u < attempted_len; ++i) {
        attempted_step_mods[i] = ui->leader_prefix_modifiers[i];
    }
    sol_ui_set_status_sequence(ui, attempted, attempted_len,
                               attempted_step_mods, mods);

    ui->leader_no_match         = false;
    ui->leader_last_invalid_key = SOL_KEY_UNKNOWN;

    /* Match the new prefix against registered flows. */
    SolCommandFlowBinding *exact_match    = NULL;
    bool                   has_deeper     = false;
    bool                   has_candidate  = false;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow,
                                        ui->leader_prefix,
                                        ui->leader_prefix_modifiers,
                                        ui->leader_prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= ui->leader_prefix_length) {
            continue;
        }
        if (flow->sequence[ui->leader_prefix_length] != key) {
            continue;
        }
        if (flow->step_modifiers[ui->leader_prefix_length] != step_mods) {
            continue;
        }

        has_candidate = true;
        if (flow->sequence_length == ui->leader_prefix_length + 1u) {
            exact_match = flow;
        } else {
            has_deeper = true;
        }
    }

    if (!has_candidate) {
        sol_ui_close_leader_popup(ui);
        return true;
    }

    if (exact_match) {
        if (exact_match->callback) {
            exact_match->callback(exact_match->action, event, exact_match->user_data);
        }
        /* Publish on the same bus as buffer/text events so plugins
           can react to commands without patching workspace.c. This
           is the primary dispatch path for config-loaded bindings
           (whose callback is NULL by design). */
        if (ui->buffers) {
            SolCommandInvokedPayload payload;
            payload.action = exact_match->action;
            sol_event_publish(sol_buffer_event_bus(ui->buffers),
                               SOL_EVENT_COMMAND_INVOKED,
                               &payload, sizeof(payload), ui);
        }
        sol_ui_close_leader_popup(ui);
        return true;
    }

    if (has_deeper && ui->leader_prefix_length < SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        ui->leader_prefix[ui->leader_prefix_length]           = key;
        ui->leader_prefix_modifiers[ui->leader_prefix_length] = step_mods;
        ui->leader_prefix_length++;
        /* Prefix grew: bump the leader-prefix revision so the popup
           builder re-runs and renders the deeper suggestion set. */
        sol_ui_bump_u32(ui->sig_leader_prefix_rev);
        return true;
    }

    sol_ui_close_leader_popup(ui);
    return true;
}

/* ------------------------------------------------------------------ */
/* Window event hooks                                                  */
/* ------------------------------------------------------------------ */

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window)
{
    if (!ui || !window || ui->primary_window != window) {
        return;
    }
    ui->primary_window         = NULL;
    ui->workspace_host         = NULL;
    ui->workspace_content_host = NULL;
    ui->tree_panel_host        = NULL;
    ui->buffer_area_host       = NULL;
    ui->popup_host             = NULL;
}

void sol_ui_system_on_window_resize(SolUISystem *ui, int width, int height)
{
    /* No pixel math here — causality reflows the title/status strips and
       content_root automatically; we only bump the window-rev signal so
       size-sensitive builders re-flow against the new metrics. */
    if (!ui) {
        return;
    }
    ui->window_w = width;
    ui->window_h = height;
    sol_ui_bump_u32(ui->sig_window_rev);
}

/* ------------------------------------------------------------------ */
/* Title-bar menu                                                      */
/* ------------------------------------------------------------------ */

static void sol_ui_menu_new_buffer_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->menu_on_new_buffer) {
        ui->menu_on_new_buffer(ui->menu_user_data);
    }
}

static void sol_ui_menu_open_file_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->menu_on_open_file) {
        ui->menu_on_open_file(ui->menu_user_data);
    }
}

static void sol_ui_menu_open_folder_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->menu_on_open_folder) {
        ui->menu_on_open_folder(ui->menu_user_data);
    }
}

static void sol_ui_menu_open_plugin_manager_action(void *user_data)
{
    sol_ui_system_open_plugin_window((SolUISystem *)user_data);
}

static void sol_ui_menu_open_settings_action(void *user_data)
{
    sol_ui_system_open_settings_window((SolUISystem *)user_data);
}

/* Ca_ClickFn-compatible wrappers for welcome-screen buttons */
static void sol_ui_welcome_click_new_buffer(Ca_Button *btn, void *user_data)
{
    (void)btn;
    sol_ui_menu_new_buffer_action(user_data);
}

static void sol_ui_welcome_click_open_file(Ca_Button *btn, void *user_data)
{
    (void)btn;
    sol_ui_menu_open_file_action(user_data);
}

static void sol_ui_welcome_click_open_folder(Ca_Button *btn, void *user_data)
{
    (void)btn;
    sol_ui_menu_open_folder_action(user_data);
}

void sol_ui_system_install_menu(SolUISystem      *ui,
                                SolUIMenuActionFn on_new_buffer,
                                SolUIMenuActionFn on_open_file,
                                SolUIMenuActionFn on_open_folder,
                                void             *user_data)
{
    if (!ui || !ui->primary_window) {
        return;
    }

    ui->menu_on_new_buffer  = on_new_buffer;
    ui->menu_on_open_file   = on_open_file;
    ui->menu_on_open_folder = on_open_folder;
    ui->menu_user_data      = user_data;

    /* Build the File menu. ca_window_set_title_bar_menus deep-copies
       these structs, so stack storage is fine. */
    Ca_MenuItemDesc sol_items[] = {
        {
            .label       = "Settings\xe2\x80\xa6",
            .action      = sol_ui_menu_open_settings_action,
            .action_data = ui,
        },
    };

    Ca_MenuItemDesc file_items[] = {
        {
            .label       = "Open File...",
            .action      = sol_ui_menu_open_file_action,
            .action_data = ui,
        },
        {
            .label       = "Open Folder...",
            .action      = sol_ui_menu_open_folder_action,
            .action_data = ui,
        },
    };

    Ca_MenuItemDesc plugin_items[] = {
        {
            .label       = "Plugin Manager\xe2\x80\xa6",
            .action      = sol_ui_menu_open_plugin_manager_action,
            .action_data = ui,
        },
    };

    Ca_MenuDesc menus[] = {
        {
            .label      = "Sol",
            .items      = sol_items,
            .item_count = (int)(sizeof(sol_items) / sizeof(sol_items[0])),
        },
        {
            .label      = "File",
            .items      = file_items,
            .item_count = (int)(sizeof(file_items) / sizeof(file_items[0])),
        },
        {
            .label      = "Plugins",
            .items      = plugin_items,
            .item_count = (int)(sizeof(plugin_items) / sizeof(plugin_items[0])),
        },
    };

    ca_window_set_title_bar_menus(ui->primary_window, menus,
                                  (int)(sizeof(menus) / sizeof(menus[0])));
}

void sol_ui_system_tick(SolUISystem *ui)
{
    (void)ui;
    /* Currently equivalent to the on_frame reaping path, but exposed
       publicly so hosts that drive the loop themselves can keep async
       UI work moving forward without depending on the primary window's
       on_frame callback. */
    sol_file_picker_tick();
    sol_ui_plugin_window_tick();
    sol_ui_settings_window_tick();
    sol_ui_search_window_tick();
}

void sol_ui_system_set_plugin_manager(SolUISystem *ui, SolPluginManager *pm)
{
    if (ui) ui->plugin_manager = pm;
}

void sol_ui_system_open_plugin_window(SolUISystem *ui)
{
    if (!ui || !ui->instance) return;
    sol_ui_plugin_window_open(ui->instance, ui->plugin_manager);
}

void sol_ui_system_set_settings(SolUISystem *ui, SolSettings *settings)
{
    if (ui) ui->settings = settings;
}

void sol_ui_system_open_settings_window(SolUISystem *ui)
{
    if (!ui || !ui->instance || !ui->settings) return;
    sol_ui_settings_window_open(ui->instance, ui->settings);
}

void sol_ui_system_open_file_search(SolUISystem *ui)
{
    sol_ui_search_window_open_files(ui);
}

void sol_ui_system_open_content_search(SolUISystem *ui)
{
    sol_ui_search_window_open_contents(ui);
}

void sol_ui_system_invalidate_buffer_area(SolUISystem *ui)
{
    /* Back-compat shim. The buffer system self-notifies through
       sig_buffer_rev on every mutation, so this should normally not be
       needed. Kept so external callers (e.g. main.c) that haven't yet
       migrated still produce a redraw — we route through the same
       buffer-rev signal the data layer uses. */
    if (ui) sol_ui_bump_u32(ui->sig_buffer_rev);
}

void sol_ui_system_window_size(const SolUISystem *ui, int *out_w, int *out_h)
{
    if (out_w) *out_w = ui ? ui->window_w : 0;
    if (out_h) *out_h = ui ? ui->window_h : 0;
}

bool sol_ui_system_buffer_area_rect(const SolUISystem *ui,
                                    float *out_x,
                                    float *out_y,
                                    float *out_w,
                                    float *out_h)
{
    return sol_ui_buffer_area_rect_internal(ui, out_x, out_y, out_w, out_h);
}

/* The status bar height is whatever sol asked causality to reserve. The
   tree panel width is the value baked into .tree-panel in style.h. Keep
   these in sync if either changes. */
#define SOL_UI_TREE_PANEL_WIDTH_PX  240

int sol_ui_system_title_bar_height(const SolUISystem *ui)
{
    (void)ui;
    return SOL_UI_TITLE_BAR_HEIGHT_PX;
}

int sol_ui_system_status_bar_height(const SolUISystem *ui)
{
    (void)ui;
    return (int)SOL_UI_STATUS_BAR_HEIGHT;
}

int sol_ui_system_tree_panel_width(const SolUISystem *ui)
{
    if (!ui || !ui->file_tree || !sol_ui_system_file_tree_visible(ui) ||
        !sol_file_tree_root(ui->file_tree)) return 0;
    return SOL_UI_TREE_PANEL_WIDTH_PX;
}

bool sol_ui_system_is_leader_active(const SolUISystem *ui)
{
    return ui ? ui->leader_active : false;
}

bool sol_ui_system_focus_leaf(SolUISystem *ui, SolBufferNodeId leaf_id)
{
    if (!ui || !ui->buffers || leaf_id == 0u) return false;
    if (ui->focus_region_callback) {
        ui->focus_region_callback(false, ui->focus_region_user_data);
    }
    /* sol_buffer_set_active_leaf self-notifies. */
    return sol_buffer_set_active_leaf(ui->buffers, leaf_id);
}

void sol_ui_system_set_focus_region_callback(SolUISystem *ui,
                                             SolUIFocusRegionFn callback,
                                             void *user_data)
{
    if (!ui) return;
    ui->focus_region_callback = callback;
    ui->focus_region_user_data = user_data;
}
