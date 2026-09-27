// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* command_panel.c — Floating which-key popup for Sol command flows.
 *
 * The panel is a bottom-right anchored vertical card that lists the
 * available next-key suggestions while the leader popup is open.
 *
 * This module only emits the panel card itself. Its container \u2014 an
 * absolute-positioned overlay covering the workspace area with
 * flex-end alignment on both axes \u2014 is the popup_host installed by
 * workspace.c (see sol_ui_build_layout). That keeps popup toggles
 * scoped to their own reactive effect: opening, closing, or advancing
 * the leader prefix does not invalidate the workspace tree.
 */

#include "sol_ui_internal.h"

#include "style.h"

#include <stdio.h>

/*
 * Emit the UI nodes for a single which-key suggestion row: a fixed-width key
 * chip on the left, the action label in the centre, and an optional "+N"
 * continuation badge on the right.
 *
 * s  The suggestion to render.
 */
static void render_suggestion_row(SolFlowSuggestion s)
{
    char key_name[32];
    if (s.modifiers != SOL_MOD_NONE) {
        sol_ui_format_modified_key(s.modifiers, s.key, key_name, sizeof(key_name));
    } else {
        sol_ui_format_key_name(s.key, key_name, sizeof(key_name));
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "cf-row",
    });

    /* Key chip (fixed-width, left). */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "cf-row-key",
    });
    ca_text(&(Ca_TextDesc){ .text = key_name, .style = "cf-row-key-text" });
    ca_div_end();

    /* Label (grows to fill remaining row width). */
    ca_text(&(Ca_TextDesc){
        .text  = s.label,
        .style = "cf-row-label",
    });

    /* Continuation indicator on the right. */
    if (s.continuation_count > 0u) {
        char more[16];
        snprintf(more, sizeof(more), "+%u", (unsigned int)s.continuation_count);
        ca_text(&(Ca_TextDesc){ .text = more, .style = "cf-row-more" });
    }

    ca_div_end(); /* cf-row */
}

/*
 * Build and emit the floating which-key panel card into the current Causality
 * frame.  Collects live suggestions from the flow registry, sizes the card to
 * fit, and renders either the suggestion list, an empty-state message, or an
 * error row when the last key had no matching binding.
 *
 * ui  The UI system providing flow registry state and window dimensions.
 */
void sol_ui_render_command_flow_panel(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    SolFlowSuggestion suggestions[SOL_UI_MAX_SUGGESTIONS];
    const size_t suggestion_count =
        sol_ui_collect_suggestions(ui, suggestions, SOL_UI_MAX_SUGGESTIONS);

    /* Panel width: prefer 320 author px, shrunk to fit narrow windows.
       ui->window_w is logical px while desc sizes are author px that
       Causality multiplies by ui_scale, so compare in author units.
       Height is left to content: padding, gaps and borders come from the
       active style (glass/Retro differ), so a hand-computed height clips
       the last row whenever a style changes them. */
    const float ui_scale = sol_ui_system_scale(ui);
    const float scale = ui_scale > 0.0f ? ui_scale : 1.0f;
    float panel_w = 320.0f;
    if (ui->window_w > 0 && (float)ui->window_w / scale < panel_w) {
        panel_w = (float)ui->window_w / scale;
    }

    /* ---- Floating panel card. The surrounding cf-overlay (absolute,
       full-coverage, flex-end aligned) is provided by ui->popup_host,
       so the panel naturally lands in the bottom-right corner without
       any pixel math against the viewport. flex-end alignment keeps the
       card from stretching; flex-grow:0 keeps it content-tall. */
    ui->command_panel_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .width     = panel_w,
        .style     = "cf-panel",
    });

    if (ui->leader_no_match) {
        char bad_key[24];
        sol_ui_format_key_name(ui->leader_last_invalid_key, bad_key, sizeof(bad_key));

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "cf-row cf-row-error",
        });
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "cf-row-key",
        });
        ca_text(&(Ca_TextDesc){ .text = bad_key, .style = "cf-row-key-text" });
        ca_div_end();
        ca_text(&(Ca_TextDesc){
            .text  = "No matching flow",
            .style = "cf-row-label cf-row-label-error",
        });
        ca_div_end();
    } else if (suggestion_count == 0u) {
        ca_text(&(Ca_TextDesc){ .text = "No bindings", .style = "cf-empty" });
    } else {
        for (size_t i = 0u; i < suggestion_count; ++i) {
            render_suggestion_row(suggestions[i]);
        }
    }

    ca_div_end(); /* cf-panel */
}
