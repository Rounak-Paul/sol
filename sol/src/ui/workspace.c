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
#include "sol_platform.h"
#include "sol_settings.h"
#include "style.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations for title-bar trampolines (Ca_MenuActionFn) */
static void sol_ui_menu_new_buffer_action(void *user_data);
static void sol_ui_menu_open_file_action(void *user_data);
static void sol_ui_menu_open_folder_action(void *user_data);
static void sol_ui_menu_open_plugin_manager_action(void *user_data);
static void sol_ui_menu_open_settings_action(void *user_data);
static bool sol_ui_dispatch_command(SolUISystem *ui,
                                    SolCommandFlowBinding *flow,
                                    const char *action,
                                    const SolInputEvent *event);
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

/* Buffer split-tree and pane-local tab geometry. */
#define SOL_UI_BUFFER_SPLIT_BAR_SIZE_PX    1.0f
#define SOL_UI_BUFFER_TAB_WIDTH_PX        140.0f
#define SOL_UI_BUFFER_TAB_LABEL_CHARS      18u

#define SOL_UI_TAB_LABEL_RING 256u
static char g_tab_label_ring[SOL_UI_TAB_LABEL_RING][64];
static size_t g_tab_label_ring_cursor;

static const char *sol_ui_tab_display_name(const char *name, bool *truncated)
{
    if (!name) name = "[No Name]";
    const size_t len = strlen(name);
    if (len <= SOL_UI_BUFFER_TAB_LABEL_CHARS) {
        if (truncated) *truncated = false;
        return name;
    }
    char *slot = g_tab_label_ring[
        g_tab_label_ring_cursor++ & (SOL_UI_TAB_LABEL_RING - 1u)];
    size_t cut = SOL_UI_BUFFER_TAB_LABEL_CHARS;
    while (cut > 0u && ((unsigned char)name[cut] & 0xC0u) == 0x80u) cut--;
    memcpy(slot, name, cut);
    memcpy(slot + cut, "...", 4u);
    if (truncated) *truncated = true;
    return slot;
}

#define SOL_UI_LABEL_NEW_BUFFER  CA_ICON_NF_COD_NEW_FILE " New Buffer"
#define SOL_UI_LABEL_OPEN_FILE   CA_ICON_NF_COD_FILE " Open File..."
#define SOL_UI_LABEL_OPEN_FOLDER CA_ICON_NF_COD_FOLDER_OPENED " Open Folder..."

/*
 * Compute the buffer area's bounding rectangle in logical pixels, accounting
 * for the title bar, status bar, and optional file-tree
 * split panel.
 *
 * ui     The UI system providing window dimensions and tree visibility.
 * out_x  Receives the left edge of the buffer area.
 * out_y  Receives the top edge of the buffer area.
 * out_w  Receives the width of the buffer area.
 * out_h  Receives the height of the buffer area.
 * Returns true when the window dimensions are valid, false otherwise.
 */
static bool sol_ui_buffer_area_rect_internal(const SolUISystem *ui,
                                             float *out_x,
                                             float *out_y,
                                             float *out_w,
                                             float *out_h)
{
    if (!ui || ui->window_w <= 0 || ui->window_h <= 0) {
        return false;
    }

    const float ui_scale = ca_window_get_scale(ui->primary_window);
    const float title_h  = ca_window_get_title_bar_height(ui->primary_window);
    const float status_h = SOL_UI_STATUS_BAR_HEIGHT * (ui_scale > 0.0f ? ui_scale : 1.0f);

    float root_x = 0.0f;
    float root_y = title_h;
    float root_w = (float)ui->window_w;
    float root_h = (float)ui->window_h - title_h - status_h;

    if (root_h < 0.0f) root_h = 0.0f;

    const bool has_left_panel = ui->active_side_panel != SOL_UI_SIDE_PANEL_TOKEN_INVALID ||
        (ui->file_tree &&
         sol_ui_system_file_tree_visible(ui) &&
         sol_file_tree_root(ui->file_tree) != NULL);
    if (has_left_panel) {
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

/* Synchronize normalized glass regions with the current workspace geometry. */
static void sol_ui_sync_bg_blur_regions(SolUISystem *ui)
{
    if (!ui || !ui->bg_effects || ui->window_w <= 0 || ui->window_h <= 0)
        return;

    const float window_w = (float)ui->window_w;
    const float window_h = (float)ui->window_h;
    const float ui_scale = ca_window_get_scale(ui->primary_window);
    const float scale    = ui_scale > 0.0f ? ui_scale : 1.0f;
    const float title_h  = ca_window_get_title_bar_height(ui->primary_window) / window_h;
    const float status_h = (SOL_UI_STATUS_BAR_HEIGHT * scale) / window_h;
    const uint32_t strong_blur_passes = sol_bg_effect_blur_passes(ui->bg_effects);
    SolBgEffectBlurRegion regions[5];
    size_t count = 0;

    regions[count++] = (SolBgEffectBlurRegion){
        .x = 0.0f, .y = 0.0f, .width = 1.0f, .height = title_h,
        .passes = strong_blur_passes,
    };
    regions[count++] = (SolBgEffectBlurRegion){
        .x = 0.0f, .y = 1.0f - status_h, .width = 1.0f, .height = status_h,
        .passes = strong_blur_passes,
    };

    float buffer_x = 0.0f;
    if (sol_ui_buffer_area_rect_internal(ui, &buffer_x, NULL, NULL, NULL) &&
        buffer_x > 0.0f) {
        const float left_w = buffer_x / window_w;
        regions[count++] = (SolBgEffectBlurRegion){
            .x = 0.0f,
            .y = title_h,
            .width = left_w,
            .height = 1.0f - title_h - status_h,
            .passes = strong_blur_passes,
        };
    }

    float editor_x = 0.0f, editor_y = 0.0f, editor_w = 0.0f, editor_h = 0.0f;
    if (sol_ui_buffer_area_rect_internal(ui, &editor_x, &editor_y,
                                         &editor_w, &editor_h) &&
        editor_w > 0.0f && editor_h > 0.0f) {
        regions[count++] = (SolBgEffectBlurRegion){
            .x = editor_x / window_w,
            .y = editor_y / window_h,
            .width = editor_w / window_w,
            .height = editor_h / window_h,
            .passes = strong_blur_passes,
        };
    }

    sol_bg_effect_set_blur_regions(ui->bg_effects, regions, count);
}

/* ------------------------------------------------------------------ */
/* Reactive helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Increment a u32 revision signal, notifying every effect that subscribed
 * to it during its last evaluation.
 *
 * sig  The signal to bump (no-op when NULL).
 */
void sol_ui_bump_u32(Ca_Signal *sig)
{
    if (!sig) {
        return;
    }
    ca_signal_set_u32(sig, ca_signal_get_u32(sig) + 1u);
}

/*
 * Show or hide the file-tree explorer panel and notify reactive subscribers
 * via sig_file_tree_visible.  No-op when the visibility is unchanged.
 *
 * ui       The UI system to update.
 * visible  Desired visibility state.
 */
void sol_ui_system_set_file_tree_visible(SolUISystem *ui, bool visible)
{
    if (!ui) return;
    if (ui->file_tree_visible == visible) return;
    ui->file_tree_visible = visible;
    sol_ui_sync_bg_blur_regions(ui);
    if (ui->sig_file_tree_visible) {
        ca_signal_set_bool(ui->sig_file_tree_visible, visible);
    }
}

/*
 * Check whether the file-tree explorer panel is currently visible.
 *
 * ui  The UI system to query.
 * Returns true if the file tree is visible and the root exists, false otherwise.
 */
bool sol_ui_system_file_tree_visible(const SolUISystem *ui)
{
    return ui ? ui->file_tree_visible : false;
}

/*
 * Select the built-in file tree as the current left-sidebar content.
 *
 * ui  The UI system whose sidebar content should change.
 */
void sol_ui_system_show_file_tree(SolUISystem *ui)
{
    if (!ui) return;

    if (ui->active_side_panel != SOL_UI_SIDE_PANEL_TOKEN_INVALID) {
        ui->active_side_panel = SOL_UI_SIDE_PANEL_TOKEN_INVALID;
        sol_ui_bump_u32(ui->sig_side_panel_rev);
    }
    sol_ui_system_set_file_tree_visible(ui, true);
}

/*
 * Check whether the built-in file tree currently owns the visible sidebar.
 *
 * ui  The UI system to query.
 * Returns true when no contributed panel is active and the file tree is shown.
 */
bool sol_ui_system_file_tree_active(const SolUISystem *ui)
{
    return ui &&
           ui->active_side_panel == SOL_UI_SIDE_PANEL_TOKEN_INVALID &&
           ui->file_tree_visible &&
           ui->file_tree &&
           sol_file_tree_root(ui->file_tree) != NULL;
}

/*
 * Get the root directory path of the currently-loaded file tree, if any.
 *
 * ui  The UI system to query.
 * Returns the root directory path, or NULL if no tree is loaded or ui is NULL.
 */
const char *sol_ui_system_file_tree_root(const SolUISystem *ui)
{
    return (ui && ui->file_tree) ? sol_file_tree_root(ui->file_tree) : NULL;
}

/* Return the registered side panel matching token, or NULL. */
static SolUISidePanel *sol_ui_find_side_panel(SolUISystem *ui,
                                              SolUISidePanelToken token)
{
    if (!ui || token == SOL_UI_SIDE_PANEL_TOKEN_INVALID) return NULL;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        SolUISidePanel *panel = &ui->side_panels[i];
        if (panel->in_use && panel->token == token) return panel;
    }
    return NULL;
}


/* ------------------------------------------------------------------ */
/* Buffer workspace visitor                                            */
/* ------------------------------------------------------------------ */

/*
 * Map a SolBufferSplitDirection to the equivalent Causality direction constant.
 *
 * direction  The Sol buffer split direction.
 * Returns    CA_HORIZONTAL for vertical splits, CA_VERTICAL for horizontal.
 */
/*
 * Convert a Sol buffer split direction to Causality's orthogonal direction.
 *
 * direction  The Sol buffer split direction.
 * Returns    CA_HORIZONTAL for vertical splits, CA_VERTICAL for horizontal splits.
 */
static int sol_ui_split_direction_to_ca(SolBufferSplitDirection direction)
{
    return direction == SOL_BUFFER_SPLIT_VERTICAL ? CA_HORIZONTAL : CA_VERTICAL;
}

/*
 * Handle buffer split-pane resize events and persist the new ratio.
 *
 * ratio      The new ratio (0.0 to 1.0) the user dragged to.
 * user_data  A SolSplitCallbackCtx carrying the UI and node context.
 */
static void sol_ui_split_on_resize(float ratio, void *user_data)
{
    SolSplitCallbackCtx *ctx = (SolSplitCallbackCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->buffers || ctx->node_id == 0u) {
        return;
    }
    sol_buffer_set_split_ratio(ctx->ui->buffers, ctx->node_id, ratio);
}

/*
 * Begin a split pane during buffer-workspace traversal and attach resize handling.
 *
 * direction  The direction of the split (vertical or horizontal).
 * ratio      The initial split ratio (0.0 to 1.0).
 * node_id    The buffer split node identifier to persist resize events.
 * user_data  A SolWorkspaceVisitorContext carrying the UI system.
 */
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
        .on_resize       = cb_ctx ? sol_ui_split_on_resize : NULL,
        .user_data       = cb_ctx,
    });
}

/*
 * End a split pane during buffer-workspace traversal.
 *
 * user_data  Unused (required by the visitor pattern).
 */
static void sol_ui_visit_end_split(void *user_data)
{
    (void)user_data;
    ca_split_end();
}

/* ------------------------------------------------------------------ */
/* Pane-click context pool                                             */
/* ------------------------------------------------------------------ */

/*
 * Allocate or grow the pane-click context pool and return the next available slot.
 *
 * ui  The UI system owning the pool.
 * Returns a pointer to an available SolPaneClickCtx, or NULL if allocation fails.
 */
static SolPaneClickCtx *sol_ui_acquire_pane_click_ctx(SolUISystem *ui)
{
    if (!ui) return NULL;
    if (ui->pane_click_ctx_count == ui->pane_click_ctx_capacity) {
        const size_t old_cap = ui->pane_click_ctx_capacity;
        if (old_cap > SIZE_MAX / 2u) return NULL;
        const size_t new_cap = old_cap ? old_cap * 2u : 32u;
        if (new_cap > SIZE_MAX / sizeof(SolPaneClickCtx *)) return NULL;
        SolPaneClickCtx **grown = (SolPaneClickCtx **)realloc(
            ui->pane_click_ctxs, new_cap * sizeof(SolPaneClickCtx *));
        if (!grown) return NULL;
        for (size_t i = old_cap; i < new_cap; ++i) {
            grown[i] = NULL;
        }
        ui->pane_click_ctxs = grown;
        ui->pane_click_ctx_capacity = new_cap;
    }
    SolPaneClickCtx **slot = &ui->pane_click_ctxs[ui->pane_click_ctx_count++];
    if (!*slot) {
        *slot = (SolPaneClickCtx *)calloc(1u, sizeof(SolPaneClickCtx));
        if (!*slot) {
            --ui->pane_click_ctx_count;
            return NULL;
        }
    }
    memset(*slot, 0, sizeof(**slot));
    return *slot;
}

/*
 * Handle a click on a buffer pane area and focus that pane's leaf.
 *
 * button     The clicked button (unused).
 * user_data  A SolPaneClickCtx identifying the pane to activate.
 */
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

/*
 * Handle a click on a buffer tab and switch to that buffer in the active leaf.
 *
 * button     The clicked button (unused).
 * user_data  A SolPaneClickCtx identifying the tab buffer and target leaf.
 */
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

/*
 * Handle a click on a tab close button and remove it from this pane.
 *
 * button     The clicked button (unused).
 * user_data  A SolPaneClickCtx identifying the buffer to close.
 */
static void sol_ui_on_tab_close(Ca_Button *button, void *user_data)
{
    (void)button;
    SolPaneClickCtx *cb = (SolPaneClickCtx *)user_data;
    if (!cb || !cb->ui || !cb->ui->buffers) return;
    if (cb->tab_buffer_id != 0u) {
        (void)sol_buffer_close_leaf_tab(cb->ui->buffers, cb->leaf_id,
                                        cb->tab_buffer_id);
    }
}

static void sol_ui_on_tab_strip_scroll(double dx, double dy, void *user_data)
{
    SolPaneClickCtx *ctx = (SolPaneClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->buffers ||
        ctx->tab_visible_count == 0u) return;
    const size_t count = sol_buffer_leaf_tab_count(
        ctx->ui->buffers, ctx->leaf_id);
    const size_t max_start = count > ctx->tab_visible_count
        ? count - ctx->tab_visible_count : 0u;
    size_t start = sol_buffer_leaf_tab_view_start(
        ctx->ui->buffers, ctx->leaf_id);
    const double motion = dx != 0.0 ? dx : -dy;
    if (motion > 0.0 && start < max_start) start++;
    else if (motion < 0.0 && start > 0u) start--;
    else return;
    (void)sol_buffer_set_leaf_tab_view_start(
        ctx->ui->buffers, ctx->leaf_id, start);
    sol_ui_system_invalidate_buffer_area(ctx->ui);
}

/*
 * Render a compact buffer tab strip for one split-pane leaf.
 *
 * Each tab belongs to this leaf only. Clicking it focuses the leaf and switches
 * its active buffer.
 *
 * ui  The UI system containing the buffers and pane context pool.
 */
static void sol_ui_render_pane_tab_strip(SolUISystem *ui,
                                         SolBufferNodeId leaf_id,
                                         const SolBufferRect *rect)
{
    if (!ui || !ui->buffers) return;
    const size_t tab_count = sol_buffer_leaf_tab_count(ui->buffers, leaf_id);
    if (tab_count == 0u) return;

    const SolBufferId active_bufid = sol_buffer_leaf_buffer(ui->buffers, leaf_id);
    float pane_width = rect ? rect->w : 0.0f;
    if (pane_width <= 0.0f) pane_width = SOL_UI_BUFFER_TAB_WIDTH_PX;
    size_t visible_count = pane_width > 5.0f
        ? (size_t)((pane_width - 5.0f) / (SOL_UI_BUFFER_TAB_WIDTH_PX + 1.0f))
        : 1u;
    if (visible_count < 1u) visible_count = 1u;
    if (visible_count > tab_count) visible_count = tab_count;

    size_t active_index = 0u;
    for (size_t i = 0u; i < tab_count; i++) {
        if (sol_buffer_leaf_tab_at(ui->buffers, leaf_id, i) == active_bufid) {
            active_index = i;
            break;
        }
    }
    size_t start = sol_buffer_leaf_tab_view_start(ui->buffers, leaf_id);
    const size_t max_start = tab_count > visible_count
        ? tab_count - visible_count : 0u;
    if (start > max_start) start = max_start;
    if (active_bufid != sol_buffer_leaf_tab_last_active(ui->buffers, leaf_id)) {
        if (active_index < start) start = active_index;
        else if (active_index >= start + visible_count)
            start = active_index - visible_count + 1u;
        (void)sol_buffer_set_leaf_tab_view_start(ui->buffers, leaf_id, start);
        sol_buffer_set_leaf_tab_last_active(ui->buffers, leaf_id, active_bufid);
    }

    SolPaneClickCtx *scroll_ctx = sol_ui_acquire_pane_click_ctx(ui);
    if (scroll_ctx) {
        scroll_ctx->ui = ui;
        scroll_ctx->leaf_id = leaf_id;
        scroll_ctx->tab_visible_count = visible_count;
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "buffer-tabs-row",
        .on_scroll = scroll_ctx ? sol_ui_on_tab_strip_scroll : NULL,
        .scroll_data = scroll_ctx,
    });
    const size_t end = start + visible_count < tab_count
        ? start + visible_count : tab_count;
    for (size_t i = start; i < end; ++i) {
        const SolBufferId tab_id = sol_buffer_leaf_tab_at(
            ui->buffers, leaf_id, i);
        if (tab_id == 0u) continue;
        const SolBuffer *tab_buf = sol_buffer_get_const(ui->buffers, tab_id);
        if (!tab_buf) continue;
        const bool tab_active = (tab_id == active_bufid);
        SolPaneClickCtx *cb = sol_ui_acquire_pane_click_ctx(ui);
        if (cb) {
            cb->ui            = ui;
            cb->leaf_id       = leaf_id;
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
        const char *full_name = sol_buffer_name(tab_buf);
        bool truncated = false;
        const char *display_name = sol_ui_tab_display_name(full_name, &truncated);
        ca_text(&(Ca_TextDesc){
            .text  = display_name,
            .style = tab_active ? "buffer-tab-text buffer-tab-text-active"
                                : "buffer-tab-text",
        });
        /* Close button — separate ctx so its click doesn't fire the tab switch. */
        SolPaneClickCtx *close_cb = sol_ui_acquire_pane_click_ctx(ui);
        if (close_cb) {
            close_cb->ui            = ui;
            close_cb->leaf_id       = leaf_id;
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
            .text  = CA_ICON_NF_COD_CLOSE,
            .style = tab_active ? "buffer-tab-close-icon buffer-tab-close-icon-active"
                                : "buffer-tab-close-icon",
        });
        ca_btn_end();  /* buffer-tab-close */
        ca_btn_end();  /* buffer-tab */
        if (truncated)
            ca_tooltip(&(Ca_TooltipDesc){ .text = full_name });
        sol_ui_attach_buffer_tab_context_menu(ui, leaf_id, tab_id);
    }
    ca_div_end();   /* buffer-tabs-row */
    /* Right-clicking unused space in the strip still exposes tab actions. */
    sol_ui_attach_buffer_tab_context_menu(ui, leaf_id, active_bufid);
}

/*
 * Render a single buffer leaf pane with its content and click handlers.
 *
 * buffer     The buffer to display, or NULL if the leaf is empty.
 * leaf_id    The leaf node identifier.
 * is_active  Whether this is the currently active leaf.
 * rect       The layout rectangle for the pane, or NULL to use dynamic layout.
 * user_data  A SolWorkspaceVisitorContext carrying the UI system.
 */
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

    sol_ui_render_pane_tab_strip(ui, leaf_id, rect);

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
            .system      = ui->buffers,
        };
        if (rect) {
            args.rect = *rect;
            args.rect.y += SOL_UI_BUFFER_TAB_STRIP_HEIGHT;
            args.rect.h -= SOL_UI_BUFFER_TAB_STRIP_HEIGHT;
            if (args.rect.h < 0.0f) args.rect.h = 0.0f;
        }
        sol_buffer_render(buffer, &args);
    }

    ca_btn_end();  /* buffer-body */
    sol_ui_attach_buffer_body_context_menu(
        ui, leaf_id, buffer ? sol_buffer_id(buffer) : 0u);
    ca_div_end();  /* buffer-pane */
}

/*
 * Render the buffer split-pane tree or the welcome screen if no buffers exist.
 *
 * Traverses the buffer workspace tree, emitting split containers and leaf panes.
 * When no buffers are open, displays an interactive welcome screen.
 *
 * ui  The UI system containing the buffers and workspace state.
 */
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
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "welcome-content",
        });

        ca_text(&(Ca_TextDesc){ .text = "Sol Editor", .style = "welcome-title" });
        ca_text(&(Ca_TextDesc){ .text = "A fast, minimal text editor.", .style = "welcome-subtitle" });

        /* Action buttons */
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "welcome-actions" });
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = SOL_UI_LABEL_NEW_BUFFER,
            .style      = "welcome-btn-primary",
            .on_click   = sol_ui_welcome_click_new_buffer,
            .click_data = ui,
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = SOL_UI_LABEL_OPEN_FILE,
            .style      = "welcome-btn",
            .on_click   = sol_ui_welcome_click_open_file,
            .click_data = ui,
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = SOL_UI_LABEL_OPEN_FOLDER,
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

        ca_div_end(); /* welcome-content */
        ca_div_end(); /* welcome-pane */
        sol_ui_attach_workspace_context_menu(ui);
        return;
    }

    SolWorkspaceVisitorContext visitor_context = { .ui = ui };

    /* Reset the per-frame split callback pool. Pointers handed out
       below stay valid until the next call to this function. */
    ui->split_callback_ctx_count = 0u;
    /* pane_click_ctx is reset by the buffer-area builder. */

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

/*
 * Handle file-tree panel resize and persist the new split ratio.
 *
 * ratio      The new ratio (0.0 to 1.0) the user dragged to.
 * user_data  The SolUISystem owning the panel.
 */
static void sol_ui_on_panel_resize(float ratio, void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui) {
        ui->tree_panel_ratio = ratio;
        sol_ui_sync_bg_blur_regions(ui);
    }
}

static void sol_ui_on_terminal_resize(float ratio, void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->terminal_mgr)
        sol_terminal_manager_set_ratio(ui->terminal_mgr, 1.0f - ratio);
}

/* ------------------------------------------------------------------ */
/* Terminal manager public API (implements sol_ui_system.h contract)   */
/* ------------------------------------------------------------------ */

void sol_ui_system_set_terminal_manager(SolUISystem *ui, SolTerminalManager *mgr)
{
    if (ui) ui->terminal_mgr = mgr;
}

SolTerminalManager *sol_ui_system_terminal_manager(const SolUISystem *ui)
{
    return ui ? ui->terminal_mgr : NULL;
}

void sol_ui_system_terminal_notify(SolUISystem *ui)
{
    if (ui) sol_ui_bump_u32(ui->sig_terminal_rev);
}

/* ------------------------------------------------------------------ */
/* Reactive content builder                                            */
/* ------------------------------------------------------------------ */

/*
 * Render the buffer pane content and, when visible, wrap it with a terminal
 * split. Handles both BOTTOM (vertical split) and RIGHT (horizontal split)
 * terminal positions.  Called from sol_ui_workspace_content_builder.
 *
 * ui           The UI system.
 * term_visible Whether the terminal panel should be shown.
 */
static void sol_ui_render_buffer_and_terminal(SolUISystem *ui, bool term_visible)
{
    if (!term_visible) {
        ui->term_panel_host    = NULL;
        ui->term_viewport_host = NULL;
        sol_ui_render_workspace_tree(ui);
        return;
    }

    SolTerminalManager *mgr = ui->terminal_mgr;
    const float term_ratio  = sol_terminal_manager_ratio(mgr);
    const float buf_ratio   = 1.0f - term_ratio;
    const int   dir = (sol_terminal_manager_position(mgr) == SOL_TERMINAL_POSITION_BOTTOM)
                        ? CA_VERTICAL : CA_HORIZONTAL;

    ca_split_begin(&(Ca_SplitDesc){
        .direction       = dir,
        .ratio           = buf_ratio,
        .min_ratio       = 0.20f,
        .max_ratio       = 0.80f,
        .bar_size        = 1.0f,
        .on_resize       = sol_ui_on_terminal_resize,
        .user_data       = ui,
    });

    /* First pane: editor buffer area */
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL });
    sol_ui_render_workspace_tree(ui);
    ca_div_end();
    sol_ui_attach_workspace_context_menu(ui);

    /* Second pane: terminal panel — retain handle for layout-height queries. */
    ui->term_panel_host =
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "term-panel" });
    sol_ui_render_terminal_panel(ui);
    ca_div_end();   /* term-panel */

    ca_split_end();
}

/* ------------------------------------------------------------------ */

/*
 * Build the reactive workspace content (file tree panel and buffer area).
 *
 * Subscribes to buffer revision, file tree revision, file tree visibility,
 * and window size signals to respond to changes. Emits the file-tree split
 * (if visible) and the pane-local tab strips and split-pane tree.
 *
 * div       The workspace content host div (unused).
 * user_data The SolUISystem containing the buffers and file tree.
 */
static void sol_ui_workspace_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }
    sol_ui_reset_context_menu_ctxs(ui);

    /* Subscribe this effect to every signal whose state this builder
       reads. Causality re-runs us exactly when one of them changes:
         - sig_buffer_rev      : the buffer/split tree (auto-bumped by sol_buffer_*)
         - sig_file_tree_rev   : the file tree contents
         - sig_file_tree_visible : explorer panel visibility
         - sig_window_rev      : window resize
         - sig_terminal_rev    : terminal cell changes, focus, or visibility */
    (void)ca_signal_get_u32(ui->sig_buffer_rev);
    (void)ca_signal_get_u32(ui->sig_file_tree_rev);
    (void)ca_signal_get_bool(ui->sig_file_tree_visible);
    (void)ca_signal_get_u32(ui->sig_window_rev);
    (void)ca_signal_get_u32(ui->sig_terminal_rev);
    (void)ca_signal_get_u32(ui->sig_side_panel_rev);

    /* Top region: optional left tree panel + buffer area (+ optional terminal).
       When the tree is visible we use ca_split_begin so the divider is
       user-draggable; when hidden we render the buffer area directly. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "workspace-main-content",
    });

    SolUISidePanel *active_panel =
        sol_ui_find_side_panel(ui, ui->active_side_panel);
    const bool has_tree_root = !active_panel &&
        (ui->file_tree &&
         sol_ui_system_file_tree_visible(ui) &&
         sol_file_tree_root(ui->file_tree) != NULL);
    const bool has_left_panel = active_panel || has_tree_root;

    const bool term_visible = ui->terminal_mgr &&
                              sol_terminal_manager_visible(ui->terminal_mgr) &&
                              sol_terminal_manager_count(ui->terminal_mgr) > 0u;

    if (has_left_panel) {
        ca_split_begin(&(Ca_SplitDesc){
            .direction      = CA_HORIZONTAL,
            .ratio          = ui->tree_panel_ratio,
            .min_ratio      = 0.10f,
            .max_ratio      = 0.50f,
            .bar_size       = 1.0f,
            .on_resize      = sol_ui_on_panel_resize,
            .user_data      = ui,
        });

        /* Left pane — active plugin panel or file tree. */
        ui->tree_panel_host = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = active_panel ? "plugin-side-panel" : "tree-panel",
        });
        if (active_panel) {
            active_panel->render(active_panel->user_data);
        } else {
            sol_ui_render_file_tree_panel_body(ui);
        }
        ca_div_end();
        if (!active_panel) {
            sol_ui_attach_explorer_empty_context_menu(
                ui, sol_file_tree_root(ui->file_tree));
        }

        /* Right pane — buffer area (+ optional terminal split) */
        ui->buffer_area_host = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "workspace-buffer-area",
        });
        ui->pane_click_ctx_count = 0u;
        sol_ui_render_buffer_and_terminal(ui, term_visible);
        ca_div_end();   /* workspace-buffer-area (right pane) */
        if (!term_visible)
            sol_ui_attach_workspace_context_menu(ui);

        ca_split_end();
    } else {
        ui->tree_panel_host = NULL;

        ui->buffer_area_host = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "workspace-buffer-area",
        });
        ui->pane_click_ctx_count = 0u;
        sol_ui_render_buffer_and_terminal(ui, term_visible);
        ca_div_end();   /* workspace-buffer-area */
        if (!term_visible)
            sol_ui_attach_workspace_context_menu(ui);
    }

    ca_div_end();   /* workspace-main-content */

    /* Command-flow popup is rendered by its own reactive host
       (ui->popup_host, mounted as a sibling of workspace_content_host).
       Toggling the popup invalidates only that host — the workspace
       tree (file tree + buffer split + tabs) is untouched. */
}

/*
 * Build the reactive command-flow popup overlay (leader prefix suggestions).
 *
 * The popup host is an absolute-positioned transparent overlay. When inactive,
 * the builder emits no children, leaving the host inert. When active, it renders
 * the command-flow panel. Subscribes to leader-active, leader-prefix-rev, and
 * flow-registry-rev signals.
 *
 * div       The popup host div (unused).
 * user_data The SolUISystem containing the leader state and command flows.
 */
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

/*
 * Build the status bar content (key hints, leader state, search status).
 *
 * Causality owns the status bar container; this builder only emits children.
 * Called from within the bar's widget context.
 *
 * window    The causality window (unused).
 * user_data The SolUISystem containing the status bar state.
 */
static void sol_ui_status_bar_builder(Ca_Window *window, void *user_data)
{
    (void)window;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }
    sol_ui_render_status_bar(ui);
}

/*
 * Handle per-frame events (reap file pickers, drive caret blink).
 *
 * Reaps closed file-picker and settings windows, bumps the buffer signal
 * for caret blink animation when a buffer is focused, and calls other
 * per-tick maintenance handlers. Called between layout and paint phases.
 *
 * user_data The SolUISystem to tick.
 */
static void sol_ui_on_frame(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }
    /* Reap closed file-picker windows. Safe even when none are open.
       Reactive scheduling is owned by causality — nothing else to
       drive from here. */
    sol_file_picker_tick();
    sol_ui_settings_window_tick();
    sol_ui_search_window_tick();

    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        SolUISidePanel *panel = &ui->side_panels[i];
        if (panel->in_use && panel->tick) panel->tick(panel->user_data);
    }

    /* The active plugin selects its animation rate. Schedule a timed frame
       without turning Causality into a continuous polling renderer. */
    const double bg_frame_interval = ui->bg_effects
        ? sol_bg_effect_active_frame_interval(ui->bg_effects)
        : 0.0;
    if (bg_frame_interval > 0.0)
        ca_instance_request_frame_after(ui->instance, bg_frame_interval);

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
        if (bg_frame_interval <= 0.0)
            ca_instance_wake();
    }

    /* Drain PTY output and rebuild on actual cell changes.
     *
     * Cursor blink uses a timer (530 ms half-period) so idle-but-focused
     * terminals only trigger one rebuild per blink toggle rather than one
     * per vsync frame.  When output IS flowing (drain returns true) we bump
     * and wake immediately so every batch of parsed output lands in the next
     * frame without extra latency. */
    if (ui->terminal_mgr &&
        sol_terminal_manager_visible(ui->terminal_mgr) &&
        sol_terminal_manager_count(ui->terminal_mgr) > 0u) {

        bool needs_rebuild = sol_terminal_manager_drain(ui->terminal_mgr);

        if (sol_terminal_manager_focused(ui->terminal_mgr)) {
            /* Blink: 530 ms on / 530 ms off — only rebuild at phase toggles. */
            static uint64_t s_last_blink_toggle_ms = 0u;
            const uint64_t  now_ms = sol_platform_now_monotonic_ns() / 1000000ull;
            if (now_ms - s_last_blink_toggle_ms >= 530u) {
                ui->term_cursor_blink_on = !ui->term_cursor_blink_on;
                s_last_blink_toggle_ms   = now_ms;
                needs_rebuild            = true;
            }
            /* Keep event loop alive so next blink fires on schedule. */
            if (bg_frame_interval <= 0.0)
                ca_instance_wake();
        } else {
            /* Cursor always shown when terminal is not focused. */
            ui->term_cursor_blink_on = true;
        }

        if (needs_rebuild)
            sol_ui_bump_u32(ui->sig_terminal_rev);
    }
}

#define TERM_CELL_H_PX  SOL_UI_TERM_CELL_H_PX
#define TERM_CELL_W_PX  SOL_UI_TERM_CELL_W_PX
#define TERM_HEADER_PX  SOL_UI_TERM_HEADER_PX
#define TERM_PAD_V_PX   SOL_UI_TERM_PAD_V_PX
#define TERM_PAD_H_PX   SOL_UI_TERM_PAD_H_PX

/*
 * Update tree-scroll signal, sticky-header width, and terminal grid size
 * before each frame tick.
 *
 * Called before ca_instance_tick every frame.  Terminal grid dimensions are
 * read directly from the Causality-computed layout of term_viewport_host (set
 * by the builder the previous frame).  Using the viewport's inner layout
 * dimensions (outer size minus CSS padding from node->desc) keeps row/col
 * computation consistent with the actual flex layout even while CSS values are
 * temporarily stale during a ui_scale slider drag.
 *
 * ui  The UI system to update.
 */
void sol_ui_system_pre_tick(SolUISystem *ui)
{
    if (!ui || !ui->primary_window || !ui->sig_tree_scroll) return;

    float sy = ca_get_scroll_y(ui->primary_window, "tree-list");
    ca_signal_set_float(ui->sig_tree_scroll, sy);

    if (ui->tree_sticky_host && ui->tree_panel_host) {
        float panel_w = ca_div_get_layout_width(ui->tree_panel_host);
        if (panel_w > 14.0f)
            ca_div_set_width(ui->tree_sticky_host, panel_w - 14.0f);
    }

    /* Resize terminal grids to match the Causality-laid-out viewport dimensions.
     * term_viewport_host is stored by the builder each frame and reflects the
     * inner content area of the term-viewport button after the previous layout
     * pass.  Reading inner dimensions directly from this node (layout_h minus
     * CSS padding) keeps row/col computation consistent with the actual flex
     * layout even while the ui_scale slider is being dragged — a period where
     * win->ui_scale is updated immediately but CSS node dimensions are rescaled
     * only after the drag ends.  Falling back to term_panel_host with scaled
     * constants when the viewport handle is unavailable covers the edge case
     * where the builder hasn't run yet (first frame before any layout pass).
     * sol_terminal_resize is idempotent — no-ops when cols/rows are unchanged. */
    if (ui->terminal_mgr &&
        sol_terminal_manager_visible(ui->terminal_mgr) &&
        sol_terminal_manager_count(ui->terminal_mgr) > 0u) {

        const float ui_scale = ca_window_get_scale(ui->primary_window);
        const float cell_h   = TERM_CELL_H_PX * ui_scale;
        const float cell_w   = TERM_CELL_W_PX * ui_scale;

        float usable_h = 0.0f, usable_w = 0.0f;
        if (ui->term_viewport_host) {
            ca_btn_get_layout_inner_size(ui->term_viewport_host,
                                         &usable_w, &usable_h);
        } else if (ui->term_panel_host) {
            const float pane_h = ca_div_get_layout_height(ui->term_panel_host);
            const float pane_w = ca_div_get_layout_width(ui->term_panel_host);
            usable_h = pane_h - TERM_HEADER_PX * ui_scale
                              - TERM_PAD_V_PX   * ui_scale * 2.0f;
            usable_w = pane_w - TERM_PAD_H_PX   * ui_scale * 2.0f;
        }

        if (usable_h >= cell_h && usable_w >= cell_w) {
            int new_rows = (int)(usable_h / cell_h);
            int new_cols = (int)(usable_w / cell_w);
            if (new_rows < 2)  new_rows = 2;
            if (new_cols < 10) new_cols = 10;

            const size_t count = sol_terminal_manager_count(ui->terminal_mgr);
            bool any_resized = false;
            for (size_t i = 0; i < count; ++i) {
                SolTerminal *t = sol_terminal_manager_at(ui->terminal_mgr, i);
                if (t && (sol_terminal_cols(t) != new_cols ||
                          sol_terminal_rows(t) != new_rows)) {
                    sol_terminal_resize(t, new_cols, new_rows);
                    any_resized = true;
                }
            }
            if (any_resized)
                sol_ui_bump_u32(ui->sig_terminal_rev);
        }
    }
}

/*
 * Change callback wired to the bg effect registry: bumps sig_bg_effect_rev so
 * dependent UI (e.g. the settings panel) re-evaluates state.
 *
 * user_data  Pointer to SolUISystem.
 */
static void sol_ui_on_bg_effect_change(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) return;
    sol_ui_bump_u32(ui->sig_bg_effect_rev);
    bool active = ui->bg_effects && sol_bg_effect_active_id(ui->bg_effects);
    if (active) ca_instance_wake();
}

/*
 * Extract the caret accent color from the theme CSS and push it to the
 * background effect registry so shader-mode effects can tint themselves.
 * Scans for ".buffer-caret { background: #rrggbb" or "rgb(r,g,b)".
 *
 * css  Full theme CSS string.
 * reg  Background effect registry; receives the extracted color.
 */
static void sol_ui_push_theme_color(const char *css, SolBgEffectRegistry *reg)
{
    if (!css || !reg) return;
    const char *p = css;
    /* Iterate over every .buffer-caret rule in the CSS; last one wins. */
    float best_r = 1.0f, best_g = 1.0f, best_b = 1.0f;
    bool found = false;
    while ((p = strstr(p, ".buffer-caret")) != NULL) {
        const char *bg = strstr(p, "background:");
        const char *end_brace = strchr(p, '}');
        if (!bg || (end_brace && bg > end_brace)) { ++p; continue; }
        bg += 11; /* skip "background:" */
        while (*bg == ' ' || *bg == '\t') ++bg;
        if (*bg == '#' && strlen(bg) >= 7) {
            unsigned int rv = 0, gv = 0, bv = 0;
            if (sscanf(bg + 1, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
                best_r = rv / 255.0f;
                best_g = gv / 255.0f;
                best_b = bv / 255.0f;
                found = true;
            }
        } else if (strncmp(bg, "rgb", 3) == 0) {
            const char *paren = strchr(bg, '(');
            if (paren) {
                int rv = 0, gv = 0, bv = 0;
                if (sscanf(paren + 1, "%d,%d,%d", &rv, &gv, &bv) == 3) {
                    best_r = rv / 255.0f;
                    best_g = gv / 255.0f;
                    best_b = bv / 255.0f;
                    found = true;
                }
            }
        }
        ++p;
    }
    if (found)
        sol_bg_effect_set_theme_color(reg, best_r, best_g, best_b);
}

/* Build and apply a stylesheet from theme CSS + appearance overlay.
 * active_id must be non-NULL; css must be the full theme CSS string.
 * Returns true on success. */
static bool sol_ui_rebuild_stylesheet(SolUISystem *ui,
                                      const char *active_id,
                                      const char *css)
{
    /* Compose: theme CSS + appearance overlay (if settings attached). */
    char *composed = NULL;
    if (ui->settings) {
        char overlay[4096];
        int olen = sol_settings_build_appearance_css(ui->settings,
                                                      overlay, (int)sizeof(overlay));
        if (olen > 0) {
            size_t tlen = strlen(css);
            composed = (char *)malloc(tlen + (size_t)olen + 1);
            if (composed) {
                memcpy(composed, css, tlen);
                memcpy(composed + tlen, overlay, (size_t)olen + 1);
            }
        }
    }

    Ca_Stylesheet *stylesheet = ca_css_parse(composed ? composed : css);
    free(composed);
    if (!stylesheet) return false;

    Ca_Stylesheet *previous = ui->stylesheet;
    ui->stylesheet = stylesheet;
    ca_instance_set_stylesheet(ui->instance, stylesheet);
    ca_instance_refresh_styles(ui->instance);
    if (previous) ca_css_destroy(previous);
    if (ui->bg_effects) sol_ui_push_theme_color(css, ui->bg_effects);
    snprintf(ui->applied_theme_id, sizeof(ui->applied_theme_id), "%s", active_id);
    return true;
}

/* Apply the active registry theme to every live Causality window. */
static void sol_ui_on_theme_change(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->instance || !ui->themes) return;
    const char *active_id = sol_theme_active_id(ui->themes);
    const char *css = sol_theme_active_css(ui->themes);
    if (!active_id || !css) return;

    sol_ui_rebuild_stylesheet(ui, active_id, css);
    sol_ui_bump_u32(ui->sig_theme_rev);
}

/* Re-apply the current stylesheet with the updated appearance overlay. */
void sol_ui_system_apply_appearance(SolUISystem *ui)
{
    if (!ui || !ui->instance || !ui->themes) return;
    const char *active_id = sol_theme_active_id(ui->themes);
    const char *css = sol_theme_active_css(ui->themes);
    if (!active_id || !css) return;

    sol_ui_rebuild_stylesheet(ui, active_id, css);
    sol_ui_bump_u32(ui->sig_theme_rev);
}

/*
 * Build and install the main UI layout tree (workspace host and nested builders).
 *
 * Constructs the app-root container, workspace host, content host with reactive
 * builder, popup overlay host, and sticky-tree header host. Installs reactive
 * builders and frame callback.
 *
 * ui  The UI system to populate with the layout.
 * Returns true on success, false if layout construction fails.
 */
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
        .z_index   = 0,
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

/*
 * Create and initialize a new Sol UI system.
 *
 * Allocates the UI system, creates reactive signals, initializes the file tree,
 * parses the stylesheet, creates the primary window, and builds the layout tree.
 * The buffer system's revision signal is attached to auto-notify on mutations.
 *
 * instance  The Causality instance to attach the window to.
 * buffers   The buffer system to manage.
 * Returns   A new SolUISystem, or NULL if initialization fails.
 */
SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers)
{
    if (!instance || !buffers) {
        return NULL;
    }

    SolUISystem *ui = (SolUISystem *)calloc(1u, sizeof(SolUISystem));
    if (!ui) {
        return NULL;
    }

    ui->instance             = instance;
    ui->buffers              = buffers;
    ui->leader_modifier      = SOL_MOD_CTRL;
    ui->status_bar_kind      = SOL_UI_STATUS_KIND_KEY;
    ui->tree_panel_ratio     = 0.20f;
    ui->file_tree_visible    = false;
    ui->term_cursor_blink_on = true;

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
    ui->sig_terminal_rev      = ca_signal_u32  (instance, 0u);
    ui->sig_side_panel_rev    = ca_signal_u32  (instance, 0u);
    ui->sig_bg_effect_rev     = ca_signal_u32  (instance, 0u);
    ui->sig_theme_rev         = ca_signal_u32  (instance, 0u);
    if (!ui->sig_buffer_rev || !ui->sig_file_tree_rev ||
        !ui->sig_file_tree_visible ||
        !ui->sig_leader_active || !ui->sig_leader_prefix_rev ||
        !ui->sig_flow_registry_rev || !ui->sig_window_rev ||
        !ui->sig_tree_scroll || !ui->sig_terminal_rev ||
        !ui->sig_side_panel_rev || !ui->sig_bg_effect_rev ||
        !ui->sig_theme_rev) {
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

    ui->themes = sol_theme_registry_create();
    if (!ui->themes ||
        !sol_theme_register(ui->themes, &(SolThemeDesc){
            .id = SOL_UI_DEFAULT_THEME_ID,
            .name = SOL_UI_DEFAULT_THEME_NAME,
            .css = SOL_UI_DEFAULT_THEME_CSS,
        })) {
        sol_theme_registry_destroy(ui->themes);
        sol_file_tree_destroy(ui->file_tree);
        free(ui);
        return NULL;
    }

    ui->stylesheet = ca_css_parse(sol_theme_active_css(ui->themes));
    if (!ui->stylesheet) {
        sol_theme_registry_destroy(ui->themes);
        sol_file_tree_destroy(ui->file_tree);
        free(ui);
        return NULL;
    }
    ca_instance_set_stylesheet(instance, ui->stylesheet);
    snprintf(ui->applied_theme_id, sizeof(ui->applied_theme_id), "%s",
             sol_theme_active_id(ui->themes));

    ui->primary_window = ca_window_create(instance, &(Ca_WindowDesc){
        .title  = SOL_UI_WINDOW_TITLE,
        .width  = SOL_UI_WINDOW_WIDTH,
        .height = SOL_UI_WINDOW_HEIGHT,
    });

    if (!ui->primary_window) {
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        sol_theme_registry_destroy(ui->themes);
        free(ui);
        return NULL;
    }

    if (!sol_ui_build_layout(ui)) {
        ca_window_destroy(ui->primary_window);
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        sol_theme_registry_destroy(ui->themes);
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
    sol_theme_set_change_callback(ui->themes, sol_ui_on_theme_change, ui);
    return ui;
}

/*
 * Destroy a Sol UI system and free its resources.
 *
 * Frees the file tree, click context pools, context menu contexts, window,
 * stylesheet, and the UI system itself. Safe to call with NULL.
 *
 * ui  The UI system to destroy.
 */
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
    if (ui->pane_click_ctxs) {
        for (size_t i = 0u; i < ui->pane_click_ctx_capacity; ++i) {
            free(ui->pane_click_ctxs[i]);
        }
    }
    free(ui->pane_click_ctxs);
    ui->pane_click_ctxs = NULL;
    if (ui->context_menu_ctxs) {
        for (size_t i = 0u; i < ui->context_menu_ctx_capacity; ++i) {
            free(ui->context_menu_ctxs[i]);
        }
    }
    free(ui->context_menu_ctxs);
    ui->context_menu_ctxs = NULL;

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
    sol_theme_registry_destroy(ui->themes);
    ui->themes = NULL;

    free(ui);
}

/*
 * Get the primary Causality window from the UI system.
 *
 * ui  The UI system to query.
 * Returns the primary window, or NULL if ui is NULL or the window is not created.
 */
Ca_Window *sol_ui_system_primary_window(SolUISystem *ui)
{
    return ui ? ui->primary_window : NULL;
}

/* ------------------------------------------------------------------ */
/* Input event routing                                                 */
/* ------------------------------------------------------------------ */

/*
 * Route an input event through the UI system's key binding and command-flow handlers.
 *
 * Manages leader-key state, prefix matching, command-flow dispatch, and status
 * bar updates. Returns true if the event was consumed (handled by a binding or
 * leader state), false to allow propagation to the app layer.
 *
 * ui     The UI system to route through.
 * event  The input event to process.
 * Returns true if the event was handled, false to allow further processing.
 */
bool sol_ui_system_handle_input_event(SolUISystem *ui, const SolInputEvent *event)
{
    if (!ui || !event) return false;

    const SolKeyCode      key  = sol_ui_normalize_flow_key(event->data.key.key);
    const SolModifierMask mods = event->data.key.modifiers;

    /* KEY_UP: only used for tap-detection on the leader key itself.
       A clean tap (leader pressed and released with no other key in between)
       opens the popup.  Any other key pressed while leader was held cancels
       the tap so Ctrl+C, Ctrl+Z, etc. are never intercepted. */
    if (event->type == SOL_INPUT_EVENT_KEY_UP) {
        if (sol_ui_is_leader_key(ui, key) && ui->leader_tap_pending) {
            ui->leader_tap_pending = false;
            if (!ui->leader_active) {
                sol_ui_open_leader_popup(ui);
            }
            return true;
        }
        ui->leader_tap_pending = false;
        return false;
    }

    if (event->type != SOL_INPUT_EVENT_KEY_DOWN) return false;

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
        ui->leader_tap_pending = false;
        if (ui->leader_active) {
            sol_ui_close_leader_popup(ui);
            return true;
        }
        return false;
    }

    /* Leader key down: arm the tap detector.  Do not open the popup yet —
       wait for the corresponding key-up to confirm it was a clean tap.
       Repeats consume the tap without opening anything. */
    if (sol_ui_is_leader_key(ui, key)) {
        if (event->data.key.repeated) {
            ui->leader_tap_pending = false;
            return true;
        }
        if (ui->leader_active) {
            /* Second tap while popup is open: close it immediately. */
            ui->leader_tap_pending = false;
            sol_ui_close_leader_popup(ui);
        } else {
            ui->leader_tap_pending = true;
        }
        return true;
    }

    /* Any other key while leader is held cancels the tap (e.g. Ctrl+C). */
    ui->leader_tap_pending = false;

    /* If the leader popup is not yet open, only chord sequences (leader
       already active) are processed below — bare keys with no leader
       involvement are not our business. */
    if (!ui->leader_active) {
        return false;
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
        (void)sol_ui_dispatch_command(ui, exact_match,
                                      exact_match->action, event);
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

/*
 * Handle window close event and invalidate UI references.
 *
 * Called when the primary window is being destroyed. Clears all cached
 * widget pointers to prevent dangling references after window destruction.
 *
 * ui      The UI system being closed.
 * window  The window being closed (must be the primary window).
 */
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

/*
 * Handle window resize event and invalidate size-sensitive layouts.
 *
 * Updates the cached window dimensions and bumps the window-revision signal
 * to trigger re-layout of size-sensitive builders. Causality automatically
 * reflows the title and status bars.
 *
 * ui      The UI system to update.
 * width   The new window width in pixels.
 * height  The new window height in pixels.
 */
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
    sol_ui_sync_bg_blur_regions(ui);
    sol_ui_bump_u32(ui->sig_window_rev);
}

/* ------------------------------------------------------------------ */
/* Title-bar menu                                                      */
/* ------------------------------------------------------------------ */

/*
 * Menu action handler for "New Buffer" (File > New or Cmd+N equivalent).
 *
 * Delegates to the installed menu callback if one exists.
 *
 * user_data  The SolUISystem owning the menu.
 */
static void sol_ui_menu_new_buffer_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->menu_on_new_buffer) {
        ui->menu_on_new_buffer(ui->menu_user_data);
    }
}

/*
 * Menu action handler for "Open File" (File > Open File).
 *
 * Delegates to the installed menu callback if one exists.
 *
 * user_data  The SolUISystem owning the menu.
 */
static void sol_ui_menu_open_file_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->menu_on_open_file) {
        ui->menu_on_open_file(ui->menu_user_data);
    }
}

/*
 * Menu action handler for "Open Folder" (File > Open Folder).
 *
 * Delegates to the installed menu callback if one exists.
 *
 * user_data  The SolUISystem owning the menu.
 */
static void sol_ui_menu_open_folder_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (ui && ui->menu_on_open_folder) {
        ui->menu_on_open_folder(ui->menu_user_data);
    }
}

/*
 * Menu action handler for "Plugin Manager" (Sol > Plugin Manager).
 *
 * Opens the plugin manager window via the UI system.
 *
 * user_data  The SolUISystem to open the plugin window in.
 */
static void sol_ui_menu_open_plugin_manager_action(void *user_data)
{
    sol_ui_system_open_plugin_window((SolUISystem *)user_data);
}

/*
 * Menu action handler for "Settings" (Sol > Settings).
 *
 * Opens the settings window via the UI system.
 *
 * user_data  The SolUISystem to open the settings window in.
 */
static void sol_ui_menu_open_settings_action(void *user_data)
{
    sol_ui_system_open_settings_window((SolUISystem *)user_data);
}

/* Dispatch a command callback and publish its action on the command event bus. */
static bool sol_ui_dispatch_command(SolUISystem *ui,
                                    SolCommandFlowBinding *flow,
                                    const char *action,
                                    const SolInputEvent *event)
{
    if (!ui || !action || !action[0]) return false;
    if (flow && flow->callback) {
        flow->callback(flow->action, event, flow->user_data);
    }
    if (!ui->buffers) return flow != NULL;

    SolCommandInvokedPayload payload = {
        .action = flow ? flow->action : action,
    };
    sol_event_publish(sol_buffer_event_bus(ui->buffers),
                      SOL_EVENT_COMMAND_INVOKED,
                      &payload, sizeof(payload), ui);
    return true;
}

/*
 * Invoke a command through the same callback and event paths as a key chord.
 *
 * ui      The UI system containing the command registry.
 * action  The command action identifier.
 * Returns true when a callback or event bus accepted the invocation.
 */
bool sol_ui_system_invoke_command(SolUISystem *ui, const char *action)
{
    if (!ui || !action || !action[0]) return false;

    SolCommandFlowBinding *flow = NULL;
    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        if (strcmp(ui->command_flows[i].action, action) == 0) {
            flow = &ui->command_flows[i];
            break;
        }
    }
    return sol_ui_dispatch_command(ui, flow, action, NULL);
}

/* Dispatch a title-bar menu item through the shared command registry. */
static void sol_ui_menu_command_action(void *user_data)
{
    SolUIMenuItem *item = (SolUIMenuItem *)user_data;
    if (item && item->in_use) {
        (void)sol_ui_system_invoke_command(item->ui, item->action);
    }
}

/*
 * Welcome-screen button click handler for "New Buffer".
 *
 * Adapts Ca_ClickFn signature to call the menu action handler.
 *
 * btn       The clicked button (unused).
 * user_data The SolUISystem.
 */
static void sol_ui_welcome_click_new_buffer(Ca_Button *btn, void *user_data)
{
    (void)btn;
    sol_ui_menu_new_buffer_action(user_data);
}

/*
 * Welcome-screen button click handler for "Open File".
 *
 * Adapts Ca_ClickFn signature to call the menu action handler.
 *
 * btn       The clicked button (unused).
 * user_data The SolUISystem.
 */
static void sol_ui_welcome_click_open_file(Ca_Button *btn, void *user_data)
{
    (void)btn;
    sol_ui_menu_open_file_action(user_data);
}

/*
 * Welcome-screen button click handler for "Open Folder".
 *
 * Adapts Ca_ClickFn signature to call the menu action handler.
 *
 * btn       The clicked button (unused).
 * user_data The SolUISystem.
 */
static void sol_ui_welcome_click_open_folder(Ca_Button *btn, void *user_data)
{
    (void)btn;
    sol_ui_menu_open_folder_action(user_data);
}

#define SOL_UI_TITLE_MENU_LIMIT 16u
#define SOL_UI_TITLE_MENU_ITEM_LIMIT 16u

typedef struct SolUIMenuBuildItem {
    char            id[64];
    char            label[64];
    Ca_MenuActionFn action;
    void           *action_data;
    bool            separator;
    int             order;
    Ca_MenuItemDesc sub_items[16];
    int             sub_item_orders[16];
    size_t          sub_item_count;
} SolUIMenuBuildItem;

typedef struct SolUIMenuBuildGroup {
    char id[32];
    char label[64];
    int  order;
    SolUIMenuBuildItem items[SOL_UI_TITLE_MENU_ITEM_LIMIT];
    Ca_MenuItemDesc descriptors[SOL_UI_TITLE_MENU_ITEM_LIMIT];
    size_t item_count;
} SolUIMenuBuildGroup;

/* Return the menu build group matching id, or NULL. */
static SolUIMenuBuildGroup *sol_ui_find_menu_group(
    SolUIMenuBuildGroup *groups,
    size_t group_count,
    const char *id)
{
    if (!groups || !id) return NULL;
    for (size_t i = 0u; i < group_count; ++i) {
        if (strcmp(groups[i].id, id) == 0) return &groups[i];
    }
    return NULL;
}

/* Append one item to a menu build group when capacity permits. */
static bool sol_ui_append_menu_build_item(SolUIMenuBuildGroup *group,
                                          const char *id,
                                          const char *label,
                                          Ca_MenuActionFn action,
                                          void *action_data,
                                          bool separator,
                                          int order)
{
    if (!group || !id || !label ||
        group->item_count >= SOL_UI_TITLE_MENU_ITEM_LIMIT) {
        return false;
    }
    size_t index = group->item_count;
    snprintf(group->items[index].id, sizeof(group->items[index].id), "%s", id);
    snprintf(group->items[index].label, sizeof(group->items[index].label), "%s", label);
    group->items[index].action = action;
    group->items[index].action_data = action_data;
    group->items[index].separator = separator;
    group->items[index].order = order;
    group->item_count++;
    return true;
}

/* Append a command item beneath a one-level submenu, creating it as needed. */
static bool sol_ui_append_submenu_build_item(SolUIMenuBuildGroup *group,
                                             const char *submenu_id,
                                             const char *submenu_label,
                                             const Ca_MenuItemDesc *item,
                                             int order)
{
    if (!group || !submenu_id || !submenu_id[0] ||
        !submenu_label || !submenu_label[0] || !item) return false;

    size_t parent = group->item_count;
    for (size_t i = 0u; i < group->item_count; ++i) {
        if (strcmp(group->items[i].id, submenu_id) == 0) {
            parent = i;
            break;
        }
    }
    if (parent == group->item_count) {
        if (!sol_ui_append_menu_build_item(group, submenu_id, submenu_label,
                                           NULL, NULL, false, order)) {
            return false;
        }
    } else if (strcmp(group->items[parent].label, submenu_label) != 0) {
        return false;
    }

    if (group->items[parent].sub_item_count >= 16u) return false;
    size_t sub = group->items[parent].sub_item_count++;
    group->items[parent].sub_items[sub] = *item;
    group->items[parent].sub_item_orders[sub] = order;
    return true;
}

/* Sort menu groups and their items by stable numeric order. */
static void sol_ui_sort_menu_build_groups(SolUIMenuBuildGroup *groups,
                                          size_t group_count)
{
    for (size_t i = 1u; i < group_count; ++i) {
        SolUIMenuBuildGroup current = groups[i];
        size_t j = i;
        while (j > 0u && groups[j - 1u].order > current.order) {
            groups[j] = groups[j - 1u];
            --j;
        }
        groups[j] = current;
    }
    for (size_t g = 0u; g < group_count; ++g) {
        SolUIMenuBuildGroup *group = &groups[g];
        for (size_t i = 1u; i < group->item_count; ++i) {
            SolUIMenuBuildItem item = group->items[i];
            int order = item.order;
            size_t j = i;
            while (j > 0u && group->items[j - 1u].order > order) {
                group->items[j] = group->items[j - 1u];
                --j;
            }
            group->items[j] = item;
        }
        for (size_t i = 0u; i < group->item_count; ++i) {
            for (size_t s = 1u; s < group->items[i].sub_item_count; ++s) {
                Ca_MenuItemDesc sub_item = group->items[i].sub_items[s];
                int order = group->items[i].sub_item_orders[s];
                size_t j = s;
                while (j > 0u &&
                       group->items[i].sub_item_orders[j - 1u] > order) {
                    group->items[i].sub_items[j] =
                        group->items[i].sub_items[j - 1u];
                    group->items[i].sub_item_orders[j] =
                        group->items[i].sub_item_orders[j - 1u];
                    --j;
                }
                group->items[i].sub_items[j] = sub_item;
                group->items[i].sub_item_orders[j] = order;
            }
        }
    }
}

/* Rebuild the complete title-bar menu from host and plugin contributions. */
static void sol_ui_rebuild_title_bar_menus(SolUISystem *ui)
{
    if (!ui || !ui->primary_window) return;

    SolUIMenuBuildGroup groups[SOL_UI_TITLE_MENU_LIMIT] = {
        { .id = "sol", .label = "Sol", .order = 100 },
        { .id = "edit", .label = "Edit", .order = 300 },
        { .id = "view", .label = "View", .order = 400 },
        { .id = "plugins", .label = "Plugins", .order = 900 },
    };
    size_t group_count = 4u;

    (void)sol_ui_append_menu_build_item(&groups[0], "new-buffer", "New Buffer",
                                        sol_ui_menu_new_buffer_action, ui,
                                        false, 10);
    (void)sol_ui_append_menu_build_item(&groups[0], "open-file", "Open File...",
                                        sol_ui_menu_open_file_action, ui,
                                        false, 20);
    (void)sol_ui_append_menu_build_item(&groups[0], "open-folder", "Open Folder...",
                                        sol_ui_menu_open_folder_action, ui,
                                        false, 30);
    (void)sol_ui_append_menu_build_item(&groups[0], "settings-separator", "",
                                        NULL, NULL, true, 40);
    (void)sol_ui_append_menu_build_item(&groups[0], "settings", "Settings...",
                                        sol_ui_menu_open_settings_action, ui,
                                        false, 50);
    (void)sol_ui_append_menu_build_item(&groups[1], "search-separator", "",
                                        NULL, NULL, true, 30);
    (void)sol_ui_append_menu_build_item(&groups[2], "plugin-separator", "",
                                        NULL, NULL, true, 1000);
    (void)sol_ui_append_menu_build_item(&groups[3], "plugin-manager",
                                        "Plugin Manager...",
                                        sol_ui_menu_open_plugin_manager_action,
                                        ui, false, 10);
    (void)sol_ui_append_menu_build_item(&groups[3], "plugin-separator", "",
                                        NULL, NULL, true, 1000);

    for (size_t i = 0u; i < SOL_UI_MAX_MENU_ITEMS; ++i) {
        SolUIMenuItem *entry = &ui->menu_items[i];
        if (!entry->in_use) continue;

        SolUIMenuBuildGroup *group =
            sol_ui_find_menu_group(groups, group_count, entry->menu_id);
        if (!group) {
            if (group_count >= SOL_UI_TITLE_MENU_LIMIT) continue;
            group = &groups[group_count++];
            snprintf(group->id, sizeof(group->id), "%s", entry->menu_id);
            snprintf(group->label, sizeof(group->label), "%s", entry->menu_label);
            group->order = entry->menu_order;
        }
        Ca_MenuItemDesc item = {
            .label = entry->label,
            .action = sol_ui_menu_command_action,
            .action_data = entry,
        };
        if (entry->submenu_id[0]) {
            (void)sol_ui_append_submenu_build_item(
                group, entry->submenu_id, entry->submenu_label,
                &item, entry->item_order);
        } else {
            (void)sol_ui_append_menu_build_item(
                group, entry->item_id, entry->label,
                sol_ui_menu_command_action, entry, false,
                entry->item_order);
        }
    }

    sol_ui_sort_menu_build_groups(groups, group_count);
    Ca_MenuDesc menus[SOL_UI_TITLE_MENU_LIMIT];
    for (size_t i = 0u; i < group_count; ++i) {
        for (size_t item = 0u; item < groups[i].item_count; ++item) {
            SolUIMenuBuildItem *source = &groups[i].items[item];
            groups[i].descriptors[item] = (Ca_MenuItemDesc){
                .label = source->label,
                .action = source->action,
                .action_data = source->action_data,
                .separator = source->separator,
                .sub_items = source->sub_item_count ? source->sub_items : NULL,
                .sub_item_count = (int)source->sub_item_count,
            };
        }
        menus[i] = (Ca_MenuDesc){
            .label = groups[i].label,
            .items = groups[i].descriptors,
            .item_count = (int)groups[i].item_count,
        };
    }
    ca_window_set_title_bar_menus(ui->primary_window, menus, (int)group_count);
}

/* Register a command-backed title-bar menu contribution. */
SolUIMenuItemToken sol_ui_system_register_menu_item(
    SolUISystem *ui,
    const SolUIMenuItemDesc *desc)
{
    if (!ui || !desc || !desc->menu_id || !desc->menu_id[0] ||
        !desc->menu_label || !desc->menu_label[0] ||
        !desc->item_id || !desc->item_id[0] ||
        !desc->label || !desc->label[0] ||
        !desc->action || !desc->action[0]) {
        return SOL_UI_MENU_ITEM_TOKEN_INVALID;
    }

    const char *canonical_label = NULL;
    int canonical_order = 0;
    size_t top_level_item_count = 0u;
    if (strcmp(desc->menu_id, "sol") == 0) {
        canonical_label = "Sol";
        canonical_order = 100;
        top_level_item_count = 5u;
    } else if (strcmp(desc->menu_id, "edit") == 0) {
        canonical_label = "Edit";
        canonical_order = 300;
        top_level_item_count = 1u;
    } else if (strcmp(desc->menu_id, "view") == 0) {
        canonical_label = "View";
        canonical_order = 400;
        top_level_item_count = 1u;
    } else if (strcmp(desc->menu_id, "plugins") == 0) {
        canonical_label = "Plugins";
        canonical_order = 900;
        top_level_item_count = 2u;
    }
    if (canonical_label && strcmp(desc->menu_label, canonical_label) != 0) {
        return SOL_UI_MENU_ITEM_TOKEN_INVALID;
    }
    const bool has_submenu_id = desc->submenu_id && desc->submenu_id[0];
    const bool has_submenu_label = desc->submenu_label && desc->submenu_label[0];
    if (has_submenu_id != has_submenu_label) {
        return SOL_UI_MENU_ITEM_TOKEN_INVALID;
    }

    SolUIMenuItem *slot = NULL;
    bool menu_exists = canonical_label != NULL;
    bool submenu_exists = false;
    size_t submenu_item_count = 0u;
    int resolved_menu_order = canonical_label ? canonical_order : desc->menu_order;
    for (size_t i = 0u; i < SOL_UI_MAX_MENU_ITEMS; ++i) {
        SolUIMenuItem *entry = &ui->menu_items[i];
        if (!entry->in_use) {
            if (!slot) slot = entry;
            continue;
        }
        if (strcmp(entry->menu_id, desc->menu_id) == 0) {
            menu_exists = true;
            resolved_menu_order = entry->menu_order;
            if (strcmp(entry->menu_label, desc->menu_label) != 0 ||
                strcmp(entry->item_id, desc->item_id) == 0) {
                return SOL_UI_MENU_ITEM_TOKEN_INVALID;
            }
            if (has_submenu_id &&
                strcmp(entry->submenu_id, desc->submenu_id) == 0 &&
                strcmp(entry->submenu_label, desc->submenu_label) != 0) {
                return SOL_UI_MENU_ITEM_TOKEN_INVALID;
            }
            if (entry->submenu_id[0]) {
                bool first_submenu_item = true;
                for (size_t j = 0u; j < i; ++j) {
                    if (ui->menu_items[j].in_use &&
                        strcmp(ui->menu_items[j].menu_id, entry->menu_id) == 0 &&
                        strcmp(ui->menu_items[j].submenu_id,
                               entry->submenu_id) == 0) {
                        first_submenu_item = false;
                        break;
                    }
                }
                if (first_submenu_item) top_level_item_count++;
                if (has_submenu_id &&
                    strcmp(entry->submenu_id, desc->submenu_id) == 0) {
                    submenu_exists = true;
                    submenu_item_count++;
                }
            } else {
                top_level_item_count++;
            }
        }
    }
    const size_t added_top_level_items = has_submenu_id && submenu_exists ? 0u : 1u;
    if (!slot ||
        top_level_item_count + added_top_level_items > SOL_UI_TITLE_MENU_ITEM_LIMIT ||
        (has_submenu_id && submenu_item_count >= 16u)) {
        return SOL_UI_MENU_ITEM_TOKEN_INVALID;
    }
    if (!menu_exists) {
        size_t menu_count = 4u;
        for (size_t i = 0u; i < SOL_UI_MAX_MENU_ITEMS; ++i) {
            SolUIMenuItem *entry = &ui->menu_items[i];
            if (!entry->in_use) continue;
            bool first = strcmp(entry->menu_id, "sol") != 0 &&
                         strcmp(entry->menu_id, "edit") != 0 &&
                         strcmp(entry->menu_id, "view") != 0 &&
                         strcmp(entry->menu_id, "plugins") != 0;
            for (size_t j = 0u; first && j < i; ++j) {
                if (ui->menu_items[j].in_use &&
                    strcmp(ui->menu_items[j].menu_id, entry->menu_id) == 0) {
                    first = false;
                }
            }
            if (first) menu_count++;
        }
        if (menu_count >= SOL_UI_TITLE_MENU_LIMIT) {
            return SOL_UI_MENU_ITEM_TOKEN_INVALID;
        }
    }

    uint32_t token = ++ui->menu_item_next_token;
    if (token == SOL_UI_MENU_ITEM_TOKEN_INVALID) {
        token = ++ui->menu_item_next_token;
    }
    memset(slot, 0, sizeof(*slot));
    slot->ui = ui;
    slot->token = token;
    snprintf(slot->menu_id, sizeof(slot->menu_id), "%s", desc->menu_id);
    snprintf(slot->menu_label, sizeof(slot->menu_label), "%s", desc->menu_label);
    snprintf(slot->item_id, sizeof(slot->item_id), "%s", desc->item_id);
    snprintf(slot->label, sizeof(slot->label), "%s", desc->label);
    snprintf(slot->action, sizeof(slot->action), "%s", desc->action);
    snprintf(slot->submenu_id, sizeof(slot->submenu_id), "%s",
             has_submenu_id ? desc->submenu_id : "");
    snprintf(slot->submenu_label, sizeof(slot->submenu_label), "%s",
             has_submenu_label ? desc->submenu_label : "");
    slot->menu_order = resolved_menu_order;
    slot->item_order = desc->item_order;
    slot->in_use = true;
    sol_ui_rebuild_title_bar_menus(ui);
    return token;
}

/* Remove a title-bar menu contribution by token. */
void sol_ui_system_unregister_menu_item(SolUISystem *ui,
                                        SolUIMenuItemToken token)
{
    if (!ui || token == SOL_UI_MENU_ITEM_TOKEN_INVALID) return;
    for (size_t i = 0u; i < SOL_UI_MAX_MENU_ITEMS; ++i) {
        if (ui->menu_items[i].in_use && ui->menu_items[i].token == token) {
            memset(&ui->menu_items[i], 0, sizeof(ui->menu_items[i]));
            sol_ui_rebuild_title_bar_menus(ui);
            return;
        }
    }
}

/*
 * Install menu callbacks and build the title-bar menu structure.
 *
 * Installs host callbacks and rebuilds all host and contributed title-bar
 * menus on the primary window.
 *
 * ui              The UI system to install the menu in.
 * on_new_buffer   Callback for "New Buffer" action, or NULL.
 * on_open_file    Callback for "Open File" action, or NULL.
 * on_open_folder  Callback for "Open Folder" action, or NULL.
 * user_data       Context passed to the menu callbacks.
 */
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

    sol_ui_rebuild_title_bar_menus(ui);
}

/*
 * Tick the UI system's async tasks (file picker, plugin window, settings, search).
 *
 * Public API for hosts driving the event loop themselves. Equivalent to the
 * on_frame reaping path but can be called independently to keep async windows
 * responsive without relying on the primary window's frame callback.
 *
 * ui  The UI system (currently unused, kept for future expansion).
 */
void sol_ui_system_tick(SolUISystem *ui)
{
    /* Currently equivalent to the on_frame reaping path, but exposed
       publicly so hosts that drive the loop themselves can keep async
       UI work moving forward without depending on the primary window's
       on_frame callback. */
    sol_file_picker_tick();
    sol_ui_plugin_window_tick();
    sol_ui_settings_window_tick();
    sol_ui_search_window_tick();
    if (ui) {
        for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
            SolUISidePanel *panel = &ui->side_panels[i];
            if (panel->in_use && panel->tick) panel->tick(panel->user_data);
        }
    }
}

/*
 * Attach a plugin manager to the UI system.
 *
 * ui  The UI system to update.
 * pm  The plugin manager, or NULL to detach.
 */
void sol_ui_system_set_plugin_manager(SolUISystem *ui, SolPluginManager *pm)
{
    if (ui) ui->plugin_manager = pm;
}

/*
 * Get the current leader modifier key for command-flow activation.
 *
 * ui  The UI system to query.
 * Returns the leader modifier mask (typically SOL_MOD_CTRL), or SOL_MOD_NONE if ui is NULL.
 */
SolModifierMask sol_ui_system_leader_modifier(const SolUISystem *ui)
{
    return ui ? ui->leader_modifier : SOL_MOD_NONE;
}

/*
 * Set the leader modifier key for command-flow activation.
 *
 * ui   The UI system to update.
 * mod  The modifier mask to use (e.g., SOL_MOD_CTRL, SOL_MOD_ALT).
 */
void sol_ui_system_set_leader_modifier(SolUISystem *ui, SolModifierMask mod)
{
    if (ui) ui->leader_modifier = mod;
}

/*
 * Open the plugin manager window.
 *
 * ui  The UI system owning the instance and plugin manager.
 */
void sol_ui_system_open_plugin_window(SolUISystem *ui)
{
    if (!ui || !ui->instance) return;
    sol_ui_plugin_window_open(ui->instance, ui->plugin_manager);
}

/*
 * Attach a settings object to the UI system.
 *
 * ui       The UI system to update.
 * settings The settings object, or NULL to detach.
 */
void sol_ui_system_set_settings(SolUISystem *ui, SolSettings *settings)
{
    if (!ui) return;
    ui->settings = settings;
    /* Apply appearance overlay now that settings are attached. */
    sol_ui_system_apply_appearance(ui);
}

/*
 * Open the settings window.
 *
 * ui  The UI system owning the instance and settings.
 */
void sol_ui_system_open_settings_window(SolUISystem *ui)
{
    if (!ui || !ui->instance || !ui->settings) return;
    sol_ui_settings_window_open(ui->instance, ui->settings, ui->bg_effects,
                                ui->sig_bg_effect_rev, ui, ui->sig_theme_rev);
}

bool sol_ui_system_register_theme(SolUISystem *ui, const SolThemeDesc *desc)
{
    if (!ui || !ui->themes || !desc || !desc->css) return false;
    if (!sol_theme_register(ui->themes, desc)) return false;
    /* Validate the composed CSS (base + override) as stored by the registry. */
    const char *composed = sol_theme_css(ui->themes, desc->id);
    Ca_Stylesheet *validation = composed ? ca_css_parse(composed) : NULL;
    if (!validation) {
        sol_theme_unregister(ui->themes, desc->id);
        return false;
    }
    ca_css_destroy(validation);
    return true;
}

bool sol_ui_system_unregister_theme(SolUISystem *ui, const char *id)
{
    if (!ui || !ui->themes || !id || strcmp(id, SOL_UI_DEFAULT_THEME_ID) == 0)
        return false;
    return sol_theme_unregister(ui->themes, id);
}

bool sol_ui_system_set_active_theme(SolUISystem *ui, const char *id)
{
    return ui && ui->themes && sol_theme_set_active(ui->themes, id);
}

const char *sol_ui_system_active_theme(const SolUISystem *ui)
{
    return ui ? sol_theme_active_id(ui->themes) : NULL;
}

size_t sol_ui_system_theme_count(const SolUISystem *ui)
{
    return ui ? sol_theme_count(ui->themes) : 0u;
}

bool sol_ui_system_theme_info(const SolUISystem *ui, size_t index,
                              const char **out_id, const char **out_name)
{
    return ui && sol_theme_get_info(ui->themes, index, out_id, out_name);
}

/*
 * Attach a background effect registry to the UI system.
 *
 * ui   The UI system.
 * reg  Effect registry, or NULL to detach.
 */
void sol_ui_system_set_bg_effects(SolUISystem *ui, SolBgEffectRegistry *reg)
{
    if (!ui) return;
    ui->bg_effects = reg;
    if (reg) {
        sol_bg_effect_set_change_callback(reg, sol_ui_on_bg_effect_change, ui);
        ca_instance_set_bg_render(ui->instance, sol_bg_effect_on_render_aux, reg);
        ca_window_set_bg_render(ui->primary_window, sol_bg_effect_on_render, reg);
        if (sol_bg_effect_active_id(reg)) ca_instance_wake();
        sol_ui_sync_bg_blur_regions(ui);
    } else {
        ca_window_set_bg_render(ui->primary_window, NULL, NULL);
        ca_instance_set_bg_render(ui->instance, NULL, NULL);
    }
}

/*
 * Return the attached background effect registry.
 *
 * ui  The UI system.
 */
SolBgEffectRegistry *sol_ui_system_bg_effects(const SolUISystem *ui)
{
    return ui ? ui->bg_effects : NULL;
}

/*
 * Open the file search window (find files by name in the current directory tree).
 *
 * ui  The UI system to open the search window for.
 */
void sol_ui_system_open_file_search(SolUISystem *ui)
{
    sol_ui_search_window_open_files(ui);
}

/*
 * Open the content search window (find text in open buffers).
 *
 * ui  The UI system to open the search window for.
 */
void sol_ui_system_open_content_search(SolUISystem *ui)
{
    sol_ui_search_window_open_contents(ui);
}

/*
 * Force a redraw of the buffer area (back-compat shim).
 *
 * The buffer system auto-notifies via sig_buffer_rev on mutations. This
 * function allows external callers (e.g., legacy code paths) to trigger
 * a manual redraw by bumping the same signal.
 *
 * ui  The UI system to invalidate.
 */
void sol_ui_system_invalidate_buffer_area(SolUISystem *ui)
{
    /* Back-compat shim. The buffer system self-notifies through
       sig_buffer_rev on every mutation, so this should normally not be
       needed. Kept so external callers (e.g. main.c) that haven't yet
       migrated still produce a redraw — we route through the same
       buffer-rev signal the data layer uses. */
    if (ui) sol_ui_bump_u32(ui->sig_buffer_rev);
}

/* Register a plugin-contributed workspace side panel. */
SolUISidePanelToken sol_ui_system_register_side_panel(
    SolUISystem *ui,
    const SolUISidePanelDesc *desc)
{
    if (!ui || !desc || !desc->id || !desc->id[0] || !desc->render) {
        return SOL_UI_SIDE_PANEL_TOKEN_INVALID;
    }

    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        if (ui->side_panels[i].in_use &&
            strcmp(ui->side_panels[i].id, desc->id) == 0) {
            return SOL_UI_SIDE_PANEL_TOKEN_INVALID;
        }
    }

    SolUISidePanel *slot = NULL;
    for (size_t i = 0u; i < SOL_UI_MAX_SIDE_PANELS; ++i) {
        if (!ui->side_panels[i].in_use) {
            slot = &ui->side_panels[i];
            break;
        }
    }
    if (!slot) return SOL_UI_SIDE_PANEL_TOKEN_INVALID;

    uint32_t token = ++ui->side_panel_next_token;
    if (token == SOL_UI_SIDE_PANEL_TOKEN_INVALID) {
        token = ++ui->side_panel_next_token;
    }

    memset(slot, 0, sizeof(*slot));
    slot->token = token;
    snprintf(slot->id, sizeof(slot->id), "%s", desc->id);
    snprintf(slot->title, sizeof(slot->title), "%s",
             desc->title ? desc->title : desc->id);
    slot->render = desc->render;
    slot->tick = desc->tick;
    slot->user_data = desc->user_data;
    slot->in_use = true;
    sol_ui_bump_u32(ui->sig_side_panel_rev);
    return token;
}

/* Remove a plugin-contributed side panel. */
void sol_ui_system_unregister_side_panel(SolUISystem *ui,
                                         SolUISidePanelToken token)
{
    SolUISidePanel *panel = sol_ui_find_side_panel(ui, token);
    if (!panel) return;
    if (ui->active_side_panel == token) {
        ui->active_side_panel = SOL_UI_SIDE_PANEL_TOKEN_INVALID;
        sol_ui_sync_bg_blur_regions(ui);
    }
    memset(panel, 0, sizeof(*panel));
    sol_ui_bump_u32(ui->sig_side_panel_rev);
}

/* Make a registered side panel visible in the workspace. */
bool sol_ui_system_show_side_panel(SolUISystem *ui,
                                   SolUISidePanelToken token)
{
    if (!sol_ui_find_side_panel(ui, token)) return false;
    if (ui->active_side_panel == token) return true;
    ui->active_side_panel = token;
    sol_ui_sync_bg_blur_regions(ui);
    sol_ui_bump_u32(ui->sig_side_panel_rev);
    return true;
}

/* Hide the active side panel when it matches token. */
void sol_ui_system_hide_side_panel(SolUISystem *ui,
                                   SolUISidePanelToken token)
{
    if (!ui || ui->active_side_panel != token) return;
    ui->active_side_panel = SOL_UI_SIDE_PANEL_TOKEN_INVALID;
    sol_ui_sync_bg_blur_regions(ui);
    sol_ui_bump_u32(ui->sig_side_panel_rev);
}

/* Return whether token currently owns the visible side panel. */
bool sol_ui_system_side_panel_visible(const SolUISystem *ui,
                                      SolUISidePanelToken token)
{
    return ui && token != SOL_UI_SIDE_PANEL_TOKEN_INVALID &&
           ui->active_side_panel == token;
}

/* Notify the workspace that a registered side panel changed. */
void sol_ui_system_notify_side_panel(SolUISystem *ui,
                                     SolUISidePanelToken token)
{
    if (sol_ui_find_side_panel(ui, token)) {
        sol_ui_bump_u32(ui->sig_side_panel_rev);
    }
}

/* Wake the Causality event loop after worker-side state publication. */
void sol_ui_system_wake(SolUISystem *ui)
{
    if (ui && ui->instance) ca_instance_wake();
}

/*
 * Get the current window dimensions.
 *
 * ui     The UI system to query.
 * out_w  Receives the window width in pixels, or 0 if ui is NULL.
 * out_h  Receives the window height in pixels, or 0 if ui is NULL.
 */
void sol_ui_system_window_size(const SolUISystem *ui, int *out_w, int *out_h)
{
    if (out_w) *out_w = ui ? ui->window_w : 0;
    if (out_h) *out_h = ui ? ui->window_h : 0;
}

/*
 * Get the bounding rectangle of the buffer display area in logical pixels.
 *
 * Accounts for title bar, status bar, tab strip, and optional file-tree panel.
 *
 * ui     The UI system to query.
 * out_x  Receives the left edge in pixels.
 * out_y  Receives the top edge in pixels.
 * out_w  Receives the width in pixels.
 * out_h  Receives the height in pixels.
 * Returns true if the window dimensions are valid, false otherwise.
 */
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

/*
 * Get the height of the title bar in pixels.
 *
 * ui  The UI system whose resolved title-bar geometry is queried.
 * Returns the current logical title-bar height in pixels.
 */
int sol_ui_system_title_bar_height(const SolUISystem *ui)
{
    if (!ui || !ui->primary_window) return 0;
    return (int)(ca_window_get_title_bar_height(ui->primary_window) + 0.5f);
}

/*
 * Get the height of the status bar in pixels.
 *
 * ui  The UI system (unused, fixed value).
 * Returns the status bar height in pixels.
 */
int sol_ui_system_status_bar_height(const SolUISystem *ui)
{
    if (!ui || !ui->primary_window) return (int)SOL_UI_STATUS_BAR_HEIGHT;
    const float sc = ca_window_get_scale(ui->primary_window);
    return (int)(SOL_UI_STATUS_BAR_HEIGHT * (sc > 0.0f ? sc : 1.0f) + 0.5f);
}

/*
 * Get the width of the file-tree panel if visible, or 0 if hidden.
 *
 * ui  The UI system to query.
 * Returns the panel width in pixels (240px if visible), or 0 if not visible.
 */
int sol_ui_system_tree_panel_width(const SolUISystem *ui)
{
    if (!ui || !ui->file_tree || !sol_ui_system_file_tree_visible(ui) ||
        !sol_file_tree_root(ui->file_tree)) return 0;
    return SOL_UI_TREE_PANEL_WIDTH_PX;
}

/*
 * Check whether the command-flow leader popup is currently open.
 *
 * ui  The UI system to query.
 * Returns true if the leader popup is active, false otherwise.
 */
bool sol_ui_system_is_leader_active(const SolUISystem *ui)
{
    return ui ? ui->leader_active : false;
}

/*
 * Focus a buffer leaf pane, activating it in the split tree.
 *
 * Invokes the focus-region callback (if installed) and sets the leaf as the
 * active pane. The buffer system auto-notifies on success.
 *
 * ui       The UI system owning the buffers.
 * leaf_id  The leaf node ID to focus.
 * Returns true on success, false if invalid leaf or no buffers.
 */
bool sol_ui_system_focus_leaf(SolUISystem *ui, SolBufferNodeId leaf_id)
{
    if (!ui || !ui->buffers || leaf_id == 0u) return false;
    if (ui->focus_region_callback) {
        ui->focus_region_callback(false, ui->focus_region_user_data);
    }
    /* sol_buffer_set_active_leaf self-notifies. */
    return sol_buffer_set_active_leaf(ui->buffers, leaf_id);
}

/*
 * Install a callback to be invoked when a pane or leaf is focused.
 *
 * The callback receives a boolean flag and user context. Used to manage
 * external UI state (e.g., hiding search or command panels) when focus shifts.
 *
 * ui        The UI system to update.
 * callback  The callback function, or NULL to uninstall.
 * user_data Context passed to the callback.
 */
void sol_ui_system_set_focus_region_callback(SolUISystem *ui,
                                             SolUIFocusRegionFn callback,
                                             void *user_data)
{
    if (!ui) return;
    ui->focus_region_callback = callback;
    ui->focus_region_user_data = user_data;
}

void sol_ui_system_set_terminal_focus_gain_callback(SolUISystem *ui,
                                                    SolUITerminalFocusGainFn callback,
                                                    void *user_data)
{
    if (!ui) return;
    ui->terminal_focus_gain_callback = callback;
    ui->terminal_focus_gain_user_data = user_data;
}

/*
 * Grant keyboard focus to the terminal and fire the focus-gain callback so
 * the application can snapshot pre-terminal focus state before it is lost.
 * All call sites that want to focus the terminal must go through here.
 *
 * ui   The UI system owning the terminal manager.
 */
void sol_ui_system_terminal_set_focused(SolUISystem *ui, bool focused)
{
    if (!ui || !ui->terminal_mgr) return;
    if (focused && ui->terminal_focus_gain_callback) {
        ui->terminal_focus_gain_callback(ui->terminal_focus_gain_user_data);
    }
    sol_terminal_manager_set_focused(ui->terminal_mgr, focused);
}

/*
 * Install a callback to be invoked when a context menu action is selected.
 *
 * The callback receives an action code, buffer/file IDs, and user context.
 * Used to respond to operations like "Copy Path" or "Rename" in context menus.
 *
 * ui        The UI system to update.
 * callback  The callback function, or NULL to uninstall.
 * user_data Context passed to the callback.
 */
void sol_ui_system_set_context_action_callback(SolUISystem *ui,
                                               SolUIContextActionFn callback,
                                               void *user_data)
{
    if (!ui) return;
    ui->context_action_callback = callback;
    ui->context_action_user_data = user_data;
}
