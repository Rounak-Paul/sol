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
 *           └── workspace-content-host  (reactive)
 *               ├── workspace-main-content (buffer split tree, flex-grow)
 *               └── command-flow panel    (only when leader_active)
 *   <causality status bar>           (system-managed; sol installs builder)
 *
 * The reactive content host is wired via ca_div_set_builder, so any
 * sol_ui_mark_workspace_dirty() call invalidates the underlying effect
 * and the next reactive flush rebuilds the content tree.
 */

#include "sol_ui_internal.h"

#include "style.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal context types                                              */
/* ------------------------------------------------------------------ */

typedef struct SolWorkspaceVisitorContext {
    SolUISystem *ui;
} SolWorkspaceVisitorContext;

/* ------------------------------------------------------------------ */
/* Workspace dirty marking                                             */
/* ------------------------------------------------------------------ */

void sol_ui_mark_workspace_dirty(SolUISystem *ui)
{
    if (!ui) {
        return;
    }
    if (ui->workspace_content_host) {
        ca_div_invalidate(ui->workspace_content_host);
        ui->workspace_dirty = false;
        return;
    }
    ui->workspace_dirty = true;
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

static void sol_ui_visit_render_leaf(SolBuffer *buffer, SolBufferNodeId leaf_id,
                                     bool is_active, void *user_data)
{
    (void)leaf_id;
    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = is_active ? "buffer-pane buffer-pane-active" : "buffer-pane",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "buffer-body",
    });

    if (buffer) {
        SolBufferRenderArgs args = {
            .is_active   = is_active,
            .ui_context  = ctx->ui,
        };
        sol_buffer_render(buffer, &args);
    }

    ca_div_end();  /* buffer-body */
    ca_div_end();  /* buffer-pane */
}

void sol_ui_render_workspace_tree(SolUISystem *ui)
{
    if (!ui || !ui->buffers) {
        return;
    }

    if (sol_buffer_count(ui->buffers) == 0u) {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "buffer-pane",
        });
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "buffer-body",
        });
        ca_div_end();
        ca_div_end();
        return;
    }

    SolWorkspaceVisitorContext visitor_context = { .ui = ui };

    /* Reset the per-frame split callback pool. Pointers handed out
       below stay valid until the next call to this function. */
    ui->split_callback_ctx_count = 0u;

    SolBufferWorkspaceVisitor visitor;
    memset(&visitor, 0, sizeof(visitor));
    visitor.begin_split  = sol_ui_visit_begin_split;
    visitor.end_split    = sol_ui_visit_end_split;
    visitor.render_leaf  = sol_ui_visit_render_leaf;

    sol_buffer_workspace_visit(ui->buffers, &visitor, &visitor_context);
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

    /* Workspace tree fills all remaining vertical space. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "workspace-main-content",
    });
    sol_ui_render_workspace_tree(ui);
    ca_div_end();

    /* Command-flow panel renders inline above the (causality) status
       bar when leader is active. As a regular flex sibling it pushes
       the workspace tree up by exactly its height — no pixel math. */
    if (ui->leader_active) {
        sol_ui_render_command_flow_panel(ui);
    }
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
    if (ui->workspace_dirty && ui->workspace_content_host) {
        ca_div_invalidate(ui->workspace_content_host);
        ui->workspace_dirty = false;
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

    ca_div_set_builder(ui->workspace_content_host, sol_ui_workspace_content_builder, ui);
    sol_ui_workspace_content_builder(ui->workspace_content_host, ui);

    ca_div_end();   /* workspace-content-host */
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
    ui->workspace_dirty = true;
    ui->status_bar_kind = SOL_UI_STATUS_KIND_KEY;

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
    sol_ui_set_status_sequence(ui, attempted, attempted_len, mods);

    ui->leader_no_match         = false;
    ui->leader_last_invalid_key = SOL_KEY_UNKNOWN;

    /* Match the new prefix against registered flows. */
    SolCommandFlowBinding *exact_match    = NULL;
    bool                   has_deeper     = false;
    bool                   has_candidate  = false;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, ui->leader_prefix, ui->leader_prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= ui->leader_prefix_length) {
            continue;
        }
        if (flow->sequence[ui->leader_prefix_length] != key) {
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
        exact_match->callback(exact_match->action, event, exact_match->user_data);
        sol_ui_close_leader_popup(ui);
        return true;
    }

    if (has_deeper && ui->leader_prefix_length < SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        ui->leader_prefix[ui->leader_prefix_length++] = key;
        sol_ui_mark_workspace_dirty(ui);
        return true;
    }

    sol_ui_close_leader_popup(ui);
    return true;
}

/* ------------------------------------------------------------------ */
/* Built-in actions                                                    */
/* ------------------------------------------------------------------ */

bool sol_ui_system_on_save_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;
    return user_data != NULL;
}

bool sol_ui_system_on_split_vertical_action(const char *action,
                                            const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }
    if (!sol_buffer_split_active(ui->buffers, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, 0u, NULL)) {
        return false;
    }
    sol_ui_mark_workspace_dirty(ui);
    return true;
}

bool sol_ui_system_on_split_horizontal_action(const char *action,
                                              const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }
    if (!sol_buffer_split_active(ui->buffers, SOL_BUFFER_SPLIT_HORIZONTAL, 0.5f, 0u, NULL)) {
        return false;
    }
    sol_ui_mark_workspace_dirty(ui);
    return true;
}

bool sol_ui_system_on_focus_next_action(const char *action,
                                        const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }
    if (!sol_buffer_focus_next_leaf(ui->buffers)) {
        return false;
    }
    sol_ui_mark_workspace_dirty(ui);
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
}

void sol_ui_system_on_window_resize(SolUISystem *ui, int width, int height)
{
    /* No pixel math here — causality reflows the title/status strips and
       content_root automatically; we only need to invalidate the
       reactive workspace builder so it redraws against the new size. */
    if (!ui) {
        return;
    }
    ui->window_w = width;
    ui->window_h = height;
    sol_ui_mark_workspace_dirty(ui);
}
