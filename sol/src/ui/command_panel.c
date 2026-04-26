// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* command_panel.c — Floating which-key popup for Sol command flows.
 *
 * The panel is a bottom-right anchored vertical card that lists the
 * available next-key suggestions while the leader popup is open. It
 * does NOT push the workspace layout: the implementation uses an
 * absolutely-positioned overlay covering the workspace_content_host's
 * client area, with end-alignment on both axes so the inner panel
 * naturally lands in the bottom-right corner regardless of window
 * size. There is no pixel math against the viewport.
 */

#include "sol_ui_internal.h"

#include "style.h"

#include <stdio.h>

static void render_suggestion_row(SolFlowSuggestion s)
{
    char key_name[24];
    sol_ui_format_key_name(s.key, key_name, sizeof(key_name));

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

void sol_ui_render_command_flow_panel(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    SolFlowSuggestion suggestions[SOL_UI_MAX_SUGGESTIONS];
    const size_t suggestion_count =
        sol_ui_collect_suggestions(ui, suggestions, SOL_UI_MAX_SUGGESTIONS);

    /* Determine row count for height calculation. error-row, empty,
       or N suggestions all collapse to >=1 row. */
    size_t row_count;
    if (ui->leader_no_match) {
        row_count = 1u;
    } else if (suggestion_count == 0u) {
        row_count = 1u; /* the "No bindings" line */
    } else {
        row_count = suggestion_count;
    }

    /* Panel width: prefer 320 logical px, but shrink to fit narrow
       windows. ui->window_w is updated from the resize callback;
       fall back to 320 when unknown (first frame, etc.). */
    float panel_w = 320.0f;
    if (ui->window_w > 0 && (float)ui->window_w < panel_w) {
        panel_w = (float)ui->window_w;
    }

    /* Panel height (logical px): padding(6+6) + N*22 + (N-1)*2 gaps.
       Causality scales this by ui_scale internally, so use logical units. */
    const float panel_h =
        12.0f + (float)row_count * 22.0f
        + (row_count > 1u ? (float)(row_count - 1u) * 2.0f : 0.0f);

    /* ---- Overlay: absolute, fills parent's content area. End-alignment
       pushes the inner panel to the bottom-right corner. The overlay
       has no background or input handlers so the underlying workspace
       remains visible and interactive everywhere outside the panel. */
    ca_div_begin(&(Ca_DivDesc){
        .direction       = CA_VERTICAL,
        .position        = CA_POSITION_ABSOLUTE,
        .pos_x           = 0.0f,
        .pos_y           = 0.0f,
        .z_index         = 50,
        .style           = "cf-overlay",
    });

    /* ---- Floating panel card. Explicit width+height keep it from
       expanding to fill the overlay so flex-end can position it at
       the bottom-right. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .width     = panel_w,
        .height    = panel_h,
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
    ca_div_end(); /* cf-overlay */
}
