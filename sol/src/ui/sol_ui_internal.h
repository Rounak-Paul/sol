// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_ui_internal.h — Private header shared by Sol's UI modules.
 *
 * Holds the SolUISystem definition and the cross-module helpers used by
 * workspace.c, command_flow.c, command_panel.c, and status_bar.c.
 *
 * This header is not part of Sol's public API. Public consumers must use
 * sol_ui_system.h.
 */

#ifndef SOL_UI_INTERNAL_H
#define SOL_UI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <causality.h>

#include "sol_file_tree.h"
#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                          */
/* ------------------------------------------------------------------ */

#define SOL_UI_MAX_COMMAND_FLOWS      64u
#define SOL_UI_MAX_SPLIT_CALLBACKS    64u
#define SOL_UI_MAX_ACTION_LEN         63u
#define SOL_UI_MAX_LABEL_LEN          95u
#define SOL_UI_MAX_FLOW_SEQUENCE_LEN  8u
#define SOL_UI_MAX_SUGGESTIONS        32u
#define SOL_UI_STATUS_TEXT_MAX_LEN    127u

/* Causality manages the title and status strips; sol only declares the
   status-bar height it wants reserved. The title bar height is fixed by
   causality itself and not visible from sol's layout code. */
#define SOL_UI_STATUS_BAR_HEIGHT      22.0f

/* Status badge kinds — single byte to keep the struct compact. */
#define SOL_UI_STATUS_KIND_KEY        'K'
#define SOL_UI_STATUS_KIND_COMMAND    'C'
#define SOL_UI_STATUS_KIND_LEADER     'L'

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolCommandFlowBinding {
    char                    action[SOL_UI_MAX_ACTION_LEN + 1u];
    char                    label[SOL_UI_MAX_LABEL_LEN + 1u];
    SolKeyCode              sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t                  sequence_length;
    SolInputActionCallback  callback;
    void                   *user_data;
} SolCommandFlowBinding;

typedef struct SolFlowSuggestion {
    SolKeyCode  key;
    const char *label;
    uint32_t    continuation_count;
} SolFlowSuggestion;

/* Persistent context handed to causality split widgets so their drag
   callback can update the buffer system's stored ratio. Pointers must
   remain stable across rebuilds, so they live in a fixed-size array on
   SolUISystem. */
typedef struct SolSplitCallbackCtx {
    struct SolUISystem *ui;
    SolBufferNodeId     node_id;
} SolSplitCallbackCtx;

/* Persistent context handed to every clickable row in the file tree
   panel. Pointers must stay stable across rebuilds (causality keeps
   button nodes alive between frames); the pool is grown on demand. */
typedef struct SolFileTreeClickCtx {
    struct SolUISystem *ui;
    size_t              row_index;
} SolFileTreeClickCtx;

/* Persistent context handed to clickable elements inside a buffer pane
   (the pane body, and per-tab buttons). When `tab_buffer_id` is 0 the
   click means "focus this pane"; when nonzero it means "switch this
   pane to that buffer (and focus it)". The pool is reset and refilled
   each time the buffer area rebuilds. */
typedef struct SolPaneClickCtx {
    struct SolUISystem *ui;
    SolBufferNodeId     leaf_id;
    SolBufferId         tab_buffer_id;
} SolPaneClickCtx;

struct SolUISystem {
    Ca_Instance      *instance;
    Ca_Window        *primary_window;
    SolBufferSystem  *buffers;

    Ca_Stylesheet    *stylesheet;

    Ca_Div           *workspace_host;
    Ca_Div           *workspace_content_host;
    /* These two are kept as struct fields purely for tooling/debug
       inspection; sol no longer invalidates them directly. State
       changes write to the signals below and the reactive runtime
       re-runs the affected builder(s). NULL until build_layout runs. */
    Ca_Div           *tree_panel_host;
    Ca_Div           *buffer_area_host;
    /* Floating which-key popup host — absolute-positioned overlay
       sibling of workspace_content_host. Its builder subscribes to
       sig_popup_version and re-runs in isolation; the workspace
       content tree is never touched on Ctrl-press. */
    Ca_Div           *popup_host;

    /* ---- Reactive state (causality fine-grained signals) ----
     *
     * In the idiomatic causality design, state IS the signal: every
     * coherent piece of UI-driving state owns a signal that builders
     * subscribe to via ca_signal_get_*. Mutations write the signal
     * (and, for cached scalars, the plain field that non-reactive code
     * paths read). There are no "invalidate" helpers — the data layer
     * self-notifies and the UI layer notifies on UI-only state.
     *
     * Signals are owned by `instance` and freed in ca_instance_destroy;
     * sol never calls ca_signal_destroy directly.
     *
     *   sig_buffer_rev        — u32 revision counter attached to
     *                           ui->buffers. Bumped by every successful
     *                           sol_buffer_* mutation.
     *   sig_file_tree_rev     — u32 revision counter attached to
     *                           ui->file_tree. Bumped on set_root /
     *                           toggle.
     *   sig_leader_active     — bool. True while the which-key popup
     *                           is open. Mirrored by the leader_active
     *                           field for non-reactive readers.
     *   sig_leader_prefix_rev — u32 revision counter bumped whenever
     *                           the leader prefix array grows / resets
     *                           / changes no-match state.
     *   sig_flow_registry_rev — u32 revision counter bumped whenever a
     *                           command flow is registered or updated.
     *   sig_window_rev        — u32 revision counter bumped on window
     *                           resize (so layout-sensitive builders
     *                           re-flow).
     */
    Ca_Signal        *sig_buffer_rev;
    Ca_Signal        *sig_file_tree_rev;
    Ca_Signal        *sig_leader_active;
    Ca_Signal        *sig_leader_prefix_rev;
    Ca_Signal        *sig_flow_registry_rev;
    Ca_Signal        *sig_window_rev;

    /* Leader / flow state. */
    SolModifierMask   leader_modifier;
    bool              leader_active;
    bool              leader_no_match;
    SolKeyCode        leader_last_invalid_key;
    SolKeyCode        leader_prefix[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t            leader_prefix_length;

    SolCommandFlowBinding command_flows[SOL_UI_MAX_COMMAND_FLOWS];
    size_t                command_flow_count;

    /* Last-known window size in logical px. Updated from the resize
       callback; consumed by the floating command panel for responsive
       width clamping. 0 until the first frame. */
    int    window_w;
    int    window_h;

    /* Status bar single-line text + kind for badge styling. */
    char   status_bar_kind;
    char   status_bar_text[SOL_UI_STATUS_TEXT_MAX_LEN + 1u];

    /* Pool of per-split callback contexts handed to causality. Reset
       at the start of each workspace visit; entries persist between
       rebuilds so dragging in-between frames still finds a valid
       pointer. */
    SolSplitCallbackCtx split_callback_ctxs[SOL_UI_MAX_SPLIT_CALLBACKS];
    size_t              split_callback_ctx_count;

    /* File tree state + click-context pool. file_tree is NULL until a
       directory has been set as the root. */
    SolFileTree                *file_tree;
    SolFileTreeClickCtx        *file_tree_click_ctxs;
    size_t                      file_tree_click_ctx_count;
    size_t                      file_tree_click_ctx_capacity;

    /* Click-context pool for buffer-pane elements (per-pane focus
       click + per-tab buttons). Reset at the start of every buffer
       area rebuild; grown on demand. */
    SolPaneClickCtx            *pane_click_ctxs;
    size_t                      pane_click_ctx_count;
    size_t                      pane_click_ctx_capacity;

    SolUIFileOpenFn  file_open_callback;
    void            *file_open_user_data;

    /* Title-bar menu callbacks. Installed via
       sol_ui_system_install_menu(); the trampolines defined in
       workspace.c look these up to dispatch to main.c. */
    SolUIMenuActionFn menu_on_open_file;
    SolUIMenuActionFn menu_on_open_folder;
    void             *menu_user_data;
};

/* ------------------------------------------------------------------ */
/* Cross-module helpers                                                */
/* ------------------------------------------------------------------ */

/* Bump a u32 revision signal: read-current + set-plus-one. Cheap when
   nothing has subscribed. Use this for UI-only revision counters
   (sig_leader_prefix_rev, sig_flow_registry_rev, sig_window_rev).
   Data-layer signals (sig_buffer_rev, sig_file_tree_rev) are bumped
   by the data systems themselves — callers don't touch them. */
void sol_ui_bump_u32(Ca_Signal *sig);

/* Key utilities (command_flow.c) */
bool        sol_ui_is_modifier_key(SolKeyCode key);
bool        sol_ui_is_leader_key(const SolUISystem *ui, SolKeyCode key);
SolKeyCode  sol_ui_normalize_flow_key(SolKeyCode key);
void        sol_ui_format_key_name(SolKeyCode key, char *out, size_t out_size);
void        sol_ui_format_modified_key(SolModifierMask modifiers, SolKeyCode key,
                                       char *out, size_t out_size);

/* Status bar (status_bar.c) */
void sol_ui_set_status_text(SolUISystem *ui, char kind, const char *text);
void sol_ui_set_status_key(SolUISystem *ui, SolKeyCode key, SolModifierMask modifiers);
void sol_ui_set_status_sequence(SolUISystem *ui, const SolKeyCode *sequence,
                                size_t length, SolModifierMask modifiers);
void sol_ui_render_status_bar(SolUISystem *ui);

/* Leader popup (command_flow.c) */
void sol_ui_open_leader_popup(SolUISystem *ui);
void sol_ui_close_leader_popup(SolUISystem *ui);

/* Flow matching (command_flow.c) */
bool   sol_ui_flow_matches_prefix(const SolCommandFlowBinding *flow,
                                  const SolKeyCode *prefix, size_t prefix_length);
size_t sol_ui_collect_suggestions(SolUISystem *ui,
                                  SolFlowSuggestion *out, size_t capacity);

/* Render passes (command_panel.c, workspace.c) */
void sol_ui_render_command_flow_panel(SolUISystem *ui);
void sol_ui_render_workspace_tree(SolUISystem *ui);
void sol_ui_render_file_tree_panel(SolUISystem *ui);
void sol_ui_render_file_tree_panel_body(SolUISystem *ui);

#endif /* SOL_UI_INTERNAL_H */
