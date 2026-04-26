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

#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                          */
/* ------------------------------------------------------------------ */

#define SOL_UI_MAX_COMMAND_FLOWS      64u
#define SOL_UI_MAX_ACTION_LEN         63u
#define SOL_UI_MAX_LABEL_LEN          95u
#define SOL_UI_MAX_FLOW_SEQUENCE_LEN  8u
#define SOL_UI_MAX_SUGGESTIONS        32u
#define SOL_UI_STATUS_TEXT_MAX_LEN    127u

/* Causality manages the title and status strips; sol only declares the
   status-bar height it wants reserved. The title bar height is fixed by
   causality itself and not visible from sol's layout code. */
#define SOL_UI_STATUS_BAR_HEIGHT      22.0f
#define SOL_UI_CF_PANEL_HEIGHT        62.0f
#define SOL_UI_CF_GRID_ROWS           3u
#define SOL_UI_CF_GRID_COLS           4u
#define SOL_UI_CF_GRID_CAPACITY       (SOL_UI_CF_GRID_ROWS * SOL_UI_CF_GRID_COLS)

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

struct SolUISystem {
    Ca_Instance      *instance;
    Ca_Window        *primary_window;
    SolBufferSystem  *buffers;

    Ca_Stylesheet    *stylesheet;

    Ca_Div           *workspace_host;
    Ca_Div           *workspace_content_host;

    /* Leader / flow state. */
    SolModifierMask   leader_modifier;
    bool              leader_active;
    bool              leader_no_match;
    SolKeyCode        leader_last_invalid_key;
    SolKeyCode        leader_prefix[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t            leader_prefix_length;

    SolCommandFlowBinding command_flows[SOL_UI_MAX_COMMAND_FLOWS];
    size_t                command_flow_count;

    /* Pending workspace rebuild flag (deferred until content_host exists). */
    bool   workspace_dirty;

    /* Status bar single-line text + kind for badge styling. */
    char   status_bar_kind;
    char   status_bar_text[SOL_UI_STATUS_TEXT_MAX_LEN + 1u];
};

/* ------------------------------------------------------------------ */
/* Cross-module helpers                                                */
/* ------------------------------------------------------------------ */

/* Workspace dirty marking: reactivity-aware (invalidates effect when host
   exists, otherwise sets a flag the on_frame hook acts on). */
void sol_ui_mark_workspace_dirty(SolUISystem *ui);

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

#endif /* SOL_UI_INTERNAL_H */
