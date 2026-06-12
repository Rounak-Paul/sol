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
#include "sol_settings.h"
#include "sol_terminal.h"
#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                          */
/* ------------------------------------------------------------------ */

#define SOL_UI_MAX_COMMAND_FLOWS      64u
#define SOL_UI_MAX_SPLIT_CALLBACKS    64u
#define SOL_UI_MAX_ACTION_LEN         63u
#define SOL_UI_MAX_LABEL_LEN          95u
/* SOL_UI_MAX_FLOW_SEQUENCE_LEN is defined in sol_ui_system.h so the
   config loader can use it without pulling this private header. */
#define SOL_UI_MAX_SUGGESTIONS        32u
#define SOL_UI_STATUS_TEXT_MAX_LEN    127u
#define SOL_UI_MAX_STATUS_SEGMENTS    16u
#define SOL_UI_MAX_CONTEXT_ACTIONS    16u
#define SOL_UI_CONTEXT_PATH_MAX       4096u

/* Causality manages the title and status strips; sol only declares the
   status-bar height it wants reserved. The title bar height is fixed by
   causality itself and not visible from sol's layout code. */
#define SOL_UI_STATUS_BAR_HEIGHT      22.0f

/* File-tree panel layout constants — kept in sync with the CSS in style.h
   so C code can compute geometry without re-parsing the stylesheet. */
#define SOL_UI_TREE_SECTION_H         28.0f   /* .tree-section-header height */
#define SOL_UI_TREE_ROOT_ROW_H        24.0f   /* .tree-root-row height        */
#define SOL_UI_TREE_ROW_H             22.0f   /* .tree-row height             */
#define SOL_UI_TREE_STICKY_TOP        (SOL_UI_TREE_SECTION_H + SOL_UI_TREE_ROOT_ROW_H)
#define SOL_UI_TREE_STICKY_MAX        8

/* Terminal panel layout constants — kept in sync with the CSS in style.h.
 * TERM_CELL_H_PX must match ca_text()'s default height: widget.c sets
 * lbl->node->desc.height = s(16.0f) for any label without an explicit height
 * or text-wrap, so each term-line's content_size is 16 * ui_scale, NOT the
 * generic leaf fallback of 20 * ui_scale. */
#define SOL_UI_TERM_CELL_H_PX         16.0f   /* .term-line row height        */
#define SOL_UI_TERM_CELL_W_PX          8.0f   /* per-cell glyph width         */
#define SOL_UI_TERM_HEADER_PX         28.0f   /* .term-header height          */
#define SOL_UI_TERM_PAD_V_PX           4.0f   /* .term-viewport padding-top/bottom */
#define SOL_UI_TERM_PAD_H_PX           6.0f   /* .term-viewport padding-left/right */

/* Status badge kinds — single byte to keep the struct compact. */
#define SOL_UI_STATUS_KIND_KEY        'K'
#define SOL_UI_STATUS_KIND_COMMAND    'C'
#define SOL_UI_STATUS_KIND_LEADER     'L'

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* Per-plugin status bar segment contributed via sol_plugin_add_status_segment. */
typedef struct SolUIStatusSegment {
    uint32_t token;                 /* assigned handle                  */
    char     text[64];              /* displayed text                   */
    char     style_class[48];       /* CSS class (empty = default)      */
    bool     in_use;
} SolUIStatusSegment;

typedef struct SolCommandFlowBinding {
    char                    action[SOL_UI_MAX_ACTION_LEN + 1u];
    char                    label[SOL_UI_MAX_LABEL_LEN + 1u];
    SolKeyCode              sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    /* Per-step modifier mask (Shift/Alt/Super only — the leader
       modifier is implicit and stripped before matching). */
    SolModifierMask         step_modifiers[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t                  sequence_length;
    SolInputActionCallback  callback;
    void                   *user_data;
} SolCommandFlowBinding;

typedef struct SolFlowSuggestion {
    SolKeyCode      key;
    SolModifierMask modifiers;   /* Shift/Alt/Super only */
    const char     *label;
    uint32_t        continuation_count;
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

/* Persistent context for sticky-ancestor header buttons.  The array is
   fixed-size (SOL_UI_TREE_STICKY_MAX) and reset at the start of every
   sticky builder run — safe because the builder only runs on scroll
   changes and the button nodes are rebuilt each time. */
typedef struct SolStickyClickCtx {
    Ca_Window  *window;
    float       target_scroll_y;   /* pre-computed: row_index * ROW_H */
} SolStickyClickCtx;

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

typedef struct SolContextMenuCtx {
    struct SolUISystem          *ui;
    SolUIContextActionRequest    request;
    SolUIContextAction           actions[SOL_UI_MAX_CONTEXT_ACTIONS];
    int                          action_count;
    char                         path[SOL_UI_CONTEXT_PATH_MAX];
} SolContextMenuCtx;

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
    /* Retained handle to the terminal pane div for layout-height queries in
       sol_ui_system_pre_tick.  Updated each builder run; NULL when the
       terminal panel is not in the current layout tree. */
    Ca_Div           *term_panel_host;
    /* Retained handle to the term-viewport Ca_Button for exact inner-size
       queries in sol_ui_system_pre_tick.  Reading inner dimensions from this
       node gives the actual usable area consistent with the current layout pass
       (including stale CSS values during a ui_scale slider drag), eliminating
       the one-frame mismatch that caused rows to overflow and be clipped.
       NULL when the terminal panel is not in the current layout tree. */
    Ca_Button        *term_viewport_host;
    /* Floating which-key popup host — absolute-positioned overlay
       sibling of workspace_content_host. Its builder subscribes to
       sig_popup_version and re-runs in isolation; the workspace
       content tree is never touched on Ctrl-press. */
    Ca_Div           *popup_host;
    /* Sticky-scroll ancestor overlay host — absolute-positioned sibling
       of workspace_content_host. Its builder subscribes only to
       sig_tree_scroll + sig_file_tree_rev and renders the ancestor-directory
       headers that "pin" at the top of the tree panel while scrolling. */
    Ca_Div           *tree_sticky_host;

    /* Terminal cursor blink phase — toggled at 530 ms intervals by on_frame.
       Initialised true (cursor visible) so the cursor appears immediately on
       focus.  Read by sol_ui_render_terminal_panel to decide whether to draw
       the cursor cell or render it as a normal cell. */
    bool              term_cursor_blink_on;

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
     *                           toggle / clear.
     *   sig_file_tree_visible — bool. True while the explorer panel
     *                           should be shown.
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
    Ca_Signal        *sig_file_tree_visible;
    Ca_Signal        *sig_leader_active;
    Ca_Signal        *sig_leader_prefix_rev;
    Ca_Signal        *sig_flow_registry_rev;
    Ca_Signal        *sig_window_rev;
    /* Float mirror of the tree-scroll-area's scroll_y, polled each frame
       by sol_ui_system_pre_tick. Builders subscribe via ca_signal_get_float
       to rebuild sticky ancestor headers without rebuilding the buffer area. */
    Ca_Signal        *sig_tree_scroll;

    /* Leader / flow state. */
    SolModifierMask   leader_modifier;
    bool              leader_active;
    /* Set on leader-key down, cleared when any other key is pressed while
       the leader is held.  On leader-key up, if still true the press was a
       clean tap → open the popup.  Prevents Ctrl+C from triggering the leader. */
    bool              leader_tap_pending;
    bool              leader_no_match;
    SolKeyCode        leader_last_invalid_key;
    SolKeyCode        leader_prefix[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    /* Modifier mask the user held at each prefix step (leader stripped). */
    SolModifierMask   leader_prefix_modifiers[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
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

    /* File tree state + click-context pool. The tree object is created
       eagerly; its mounted root is managed by SolFileTree itself, and
       panel visibility is tracked separately here. */
    SolFileTree                *file_tree;
    bool                        file_tree_visible;
    SolFileTreeClickCtx        *file_tree_click_ctxs;
    size_t                      file_tree_click_ctx_count;
    size_t                      file_tree_click_ctx_capacity;

    /* Sticky-ancestor click context pool.  Fixed-size; reset each frame. */
    SolStickyClickCtx           sticky_click_ctxs[SOL_UI_TREE_STICKY_MAX];
    int                         sticky_click_ctx_count;

    /* Click-context pool for buffer-pane elements (per-pane focus
       click + per-tab buttons). Reset at the start of every buffer
       area rebuild; grown on demand. */
    SolPaneClickCtx            *pane_click_ctxs;
    size_t                      pane_click_ctx_count;
    size_t                      pane_click_ctx_capacity;

    SolContextMenuCtx          **context_menu_ctxs;
    size_t                       context_menu_ctx_count;
    size_t                       context_menu_ctx_capacity;

    SolUIFileOpenFn  file_open_callback;
    void            *file_open_user_data;
    SolUIFocusRegionFn focus_region_callback;
    void              *focus_region_user_data;
    SolUITerminalFocusGainFn terminal_focus_gain_callback;
    void                    *terminal_focus_gain_user_data;
    SolUIContextActionFn context_action_callback;
    void                *context_action_user_data;

    /* Title-bar menu callbacks. Installed via
       sol_ui_system_install_menu(); the trampolines defined in
       workspace.c look these up to dispatch to main.c. */
    SolUIMenuActionFn menu_on_new_buffer;
    SolUIMenuActionFn menu_on_open_file;
    SolUIMenuActionFn menu_on_open_folder;
    void             *menu_user_data;

    /* Ratio of the file-tree panel vs. the buffer area (0.0–1.0).
       Persisted here so ca_split_begin re-reads it each build and the
       panel keeps its width across reactive rebuilds. Default 0.20. */
    float tree_panel_ratio;

    /* Terminal manager — NULL until sol_ui_system_set_terminal_manager is called. */
    SolTerminalManager *terminal_mgr;
    /* Bumped by sol_ui_on_frame whenever the terminal manager drains new data,
       or when terminal focus state changes.  The workspace content builder
       subscribes to force a repaint. */
    Ca_Signal          *sig_terminal_rev;

    /* Plugin-contributed status bar segments (right side). */
    SolUIStatusSegment plugin_status_segs[SOL_UI_MAX_STATUS_SEGMENTS];
    uint32_t           plugin_status_next_token;   /* monotonic counter, starts at 1 */

    /* Back-pointer to the plugin manager, set via
       sol_ui_system_set_plugin_manager().  NULL until attached. */
    struct SolPluginManager *plugin_manager;

    /* Pointer to the application's SolSettings, set via
       sol_ui_system_set_settings().  NULL until attached. */
    SolSettings *settings;
};

/* ------------------------------------------------------------------ */
/* Cross-module helpers                                                */
/* ------------------------------------------------------------------ */

/*
 * Increment a u32 revision signal (read current + write incremented value).
 *
 * sig  Causality signal to increment.
 */
void sol_ui_bump_u32(Ca_Signal *sig);

/*
 * Open the search window in file mode (search for files by name).
 *
 * ui  UI system.
 */
void sol_ui_search_window_open_files(SolUISystem *ui);

/*
 * Open the search window in content mode (search file contents).
 *
 * ui  UI system.
 */
void sol_ui_search_window_open_contents(SolUISystem *ui);

/*
 * Update the search window each frame (process input, render).
 */
void sol_ui_search_window_tick(void);

/*
 * Check if a key code represents a modifier key (Shift, Alt, Control, etc.).
 *
 * key  Key code to test.
 * Returns True if the key is a modifier.
 */
bool        sol_ui_is_modifier_key(SolKeyCode key);

/*
 * Check if a key code matches the UI system's leader key.
 *
 * ui   UI system.
 * key  Key code to test.
 * Returns True if the key matches the configured leader.
 */
bool        sol_ui_is_leader_key(const SolUISystem *ui, SolKeyCode key);

/*
 * Normalize a key code for command flow matching (strip redundant variants).
 *
 * key  Key code to normalize.
 * Returns Normalized key code.
 */
SolKeyCode  sol_ui_normalize_flow_key(SolKeyCode key);

/*
 * Format a key code into a human-readable name string.
 *
 * key       Key code to format.
 * out       Output buffer.
 * out_size  Size of the output buffer.
 */
void        sol_ui_format_key_name(SolKeyCode key, char *out, size_t out_size);

/*
 * Format a modifier mask and key code into a combined readable string.
 *
 * modifiers  Modifier mask (Shift, Alt, Control).
 * key        Key code.
 * out        Output buffer.
 * out_size   Size of the output buffer.
 */
void        sol_ui_format_modified_key(SolModifierMask modifiers, SolKeyCode key,
                                       char *out, size_t out_size);

/*
 * Set the status bar text and kind badge.
 *
 * ui    UI system.
 * kind  Single character badge kind (K/C/L).
 * text  Status text to display.
 */
void sol_ui_set_status_text(SolUISystem *ui, char kind, const char *text);

/*
 * Set the status bar to display a single key binding.
 *
 * ui        UI system.
 * key       Key code to display.
 * modifiers Modifier mask for the key.
 */
void sol_ui_set_status_key(SolUISystem *ui, SolKeyCode key, SolModifierMask modifiers);

/*
 * Set the status bar to display a key sequence binding.
 *
 * ui               UI system.
 * sequence         Array of key codes in the sequence.
 * length           Number of keys in the sequence.
 * step_modifiers   Optional modifier mask per step.
 * last_modifiers   Final modifier mask for completion display.
 */
void sol_ui_set_status_sequence(SolUISystem *ui, const SolKeyCode *sequence,
                                size_t length,
                                const SolModifierMask *step_modifiers,
                                SolModifierMask last_modifiers);

/*
 * Render the status bar to the causality widget tree.
 *
 * ui  UI system.
 */
void sol_ui_render_status_bar(SolUISystem *ui);

/*
 * Open the leader (which-key) popup overlay.
 *
 * ui  UI system.
 */
void sol_ui_open_leader_popup(SolUISystem *ui);

/*
 * Close the leader (which-key) popup overlay.
 *
 * ui  UI system.
 */
void sol_ui_close_leader_popup(SolUISystem *ui);

/*
 * Check if a command flow matches the given key prefix.
 *
 * flow             Command flow to test.
 * prefix           Key codes in the prefix.
 * prefix_modifiers Modifier masks per prefix step (may be NULL).
 * prefix_length    Number of keys in the prefix.
 * Returns True if the flow matches the prefix.
 */
bool   sol_ui_flow_matches_prefix(const SolCommandFlowBinding *flow,
                                  const SolKeyCode *prefix,
                                  const SolModifierMask *prefix_modifiers,
                                  size_t prefix_length);

/*
 * Collect all viable next key suggestions for the current leader prefix.
 *
 * ui        UI system.
 * out       Output array for suggestions.
 * capacity  Maximum number of suggestions to return.
 * Returns Number of suggestions collected.
 */
size_t sol_ui_collect_suggestions(SolUISystem *ui,
                                  SolFlowSuggestion *out, size_t capacity);

/*
 * Render the command flow (leader) popup panel.
 *
 * ui  UI system.
 */
void sol_ui_render_command_flow_panel(SolUISystem *ui);

/*
 * Render the workspace tree structure (buffers and splits).
 *
 * ui  UI system.
 */
void sol_ui_render_workspace_tree(SolUISystem *ui);

/*
 * Render the integrated terminal panel (tab strip + cell viewport).
 * Called from workspace.c when the terminal manager is visible.
 *
 * ui  UI system (terminal_mgr must be non-NULL).
 */
void sol_ui_render_terminal_panel(SolUISystem *ui);

/*
 * Render the file tree explorer panel (top-level).
 *
 * ui  UI system.
 */
void sol_ui_render_file_tree_panel(SolUISystem *ui);

/*
 * Render the file tree explorer panel body (tree rows).
 *
 * ui  UI system.
 */
void sol_ui_render_file_tree_panel_body(SolUISystem *ui);

/*
 * Render builder for sticky-scroll ancestor headers in the file tree.
 *
 * div       Causality div element to populate.
 * user_data User data (cast to SolUISystem*).
 */
void sol_ui_sticky_tree_builder(Ca_Div *div, void *user_data);

/*
 * Allocate a context menu context from the pool.
 *
 * ui  UI system.
 * Returns Newly allocated context menu context (or existing if pool full).
 */
SolContextMenuCtx *sol_ui_acquire_context_menu_ctx(SolUISystem *ui);

/*
 * Clear all context menu contexts in the pool.
 *
 * ui  UI system.
 */
void sol_ui_reset_context_menu_ctxs(SolUISystem *ui);

/*
 * Attach a context menu to the workspace background.
 *
 * ui  UI system.
 */
void sol_ui_attach_workspace_context_menu(SolUISystem *ui);

/*
 * Attach a context menu to an explorer root folder.
 *
 * ui         UI system.
 * root_path  Path to the root folder.
 */
void sol_ui_attach_explorer_root_context_menu(SolUISystem *ui, const char *root_path);

/*
 * Attach a context menu to the empty explorer area.
 *
 * ui         UI system.
 * root_path  Path to the root folder context.
 */
void sol_ui_attach_explorer_empty_context_menu(SolUISystem *ui, const char *root_path);

/*
 * Attach a context menu to a file tree item.
 *
 * ui      UI system.
 * path    Full path to the file or directory.
 * is_dir  True if the item is a directory.
 */
void sol_ui_attach_explorer_item_context_menu(SolUISystem *ui,
                                              const char *path,
                                              bool is_dir);

/*
 * Attach a context menu to a buffer pane body (editor area).
 *
 * ui        UI system.
 * leaf_id   ID of the buffer node (pane).
 * buffer_id ID of the buffer in the pane.
 */
void sol_ui_attach_buffer_body_context_menu(SolUISystem *ui,
                                            SolBufferNodeId leaf_id,
                                            SolBufferId buffer_id);

/*
 * Attach a context menu to a buffer tab.
 *
 * ui        UI system.
 * leaf_id   ID of the buffer node (pane).
 * buffer_id ID of the buffer whose tab was clicked.
 */
void sol_ui_attach_buffer_tab_context_menu(SolUISystem *ui,
                                           SolBufferNodeId leaf_id,
                                           SolBufferId buffer_id);

/*
 * Open the plugin manager window.
 *
 * instance  Causality instance for the window.
 * pm        Plugin manager to display.
 */
void sol_ui_plugin_window_open(Ca_Instance *instance, SolPluginManager *pm);

/*
 * Update the plugin manager window each frame.
 */
void sol_ui_plugin_window_tick(void);

/*
 * Open the settings window.
 *
 * instance  Causality instance for the window.
 * settings  Settings object to edit.
 */
void sol_ui_settings_window_open(Ca_Instance *instance, SolSettings *settings);

/*
 * Update the settings window each frame.
 */
void sol_ui_settings_window_tick(void);

#endif /* SOL_UI_INTERNAL_H */
