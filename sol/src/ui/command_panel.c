// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* command_panel.c — NvChad-style which-key popup for Sol command flows.
 *
 * Renders the panel that sits above the (causality-managed) status bar
 * and lists available next-key suggestions while the leader popup is
 * open. The panel is just a regular flex sibling of the workspace tree:
 * causality reflows the workspace to make room for it, so there is no
 * pixel math against viewport size or status-bar height in this file.
 * A 3x4 grid keeps the layout stable as the user types deeper.
 */

#include "sol_ui_internal.h"

#include "style.h"

#include <stdio.h>

void sol_ui_render_command_flow_panel(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    SolFlowSuggestion suggestions[SOL_UI_MAX_SUGGESTIONS];
    const size_t suggestion_count =
        sol_ui_collect_suggestions(ui, suggestions, SOL_UI_MAX_SUGGESTIONS);

    /* Inline flex panel — width inherits parent, height fixed.
       width = 0 → fill parent; do NOT pre-multiply by ui_scale. */
    ca_div_begin(&(Ca_DivDesc){
        .direction    = CA_VERTICAL,
        .height       = SOL_UI_CF_PANEL_HEIGHT,
        .border_width = 1.0f,
        .border_color = ca_color(0.14f, 0.21f, 0.32f, 1.0f),
        .style        = "cf-panel",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "cf-grid",
    });

    if (ui->leader_no_match) {
        char bad_key[24];
        sol_ui_format_key_name(ui->leader_last_invalid_key, bad_key, sizeof(bad_key));

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "cf-grid-row" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "cf-cell cf-chip-error" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "cf-chip-key-group" });
        ca_text(&(Ca_TextDesc){ .text = bad_key, .style = "cf-chip-key" });
        ca_div_end();
        ca_text(&(Ca_TextDesc){ .text = "No matching flow", .style = "cf-chip-label cf-label-error" });
        ca_div_end();
        ca_div_end();
    } else if (suggestion_count == 0u) {
        ca_text(&(Ca_TextDesc){ .text = "No bindings", .style = "cf-empty" });
    } else {
        const size_t max_items = suggestion_count < SOL_UI_CF_GRID_CAPACITY
            ? suggestion_count
            : SOL_UI_CF_GRID_CAPACITY;

        for (size_t row = 0u; row < SOL_UI_CF_GRID_ROWS; ++row) {
            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_HORIZONTAL,
                .style     = "cf-grid-row",
            });

            for (size_t col = 0u; col < SOL_UI_CF_GRID_COLS; ++col) {
                const size_t idx = row * SOL_UI_CF_GRID_COLS + col;

                ca_div_begin(&(Ca_DivDesc){
                    .direction = CA_HORIZONTAL,
                    .style     = "cf-cell",
                });

                if (idx < max_items) {
                    char key_name[24];
                    sol_ui_format_key_name(suggestions[idx].key, key_name, sizeof(key_name));

                    ca_div_begin(&(Ca_DivDesc){
                        .direction = CA_HORIZONTAL,
                        .style     = "cf-chip-key-group",
                    });
                    ca_text(&(Ca_TextDesc){ .text = key_name, .style = "cf-chip-key" });
                    if (suggestions[idx].continuation_count > 0u) {
                        char more[16];
                        snprintf(more, sizeof(more), "+%u",
                                 (unsigned int)suggestions[idx].continuation_count);
                        ca_text(&(Ca_TextDesc){ .text = more, .style = "cf-chip-more" });
                    }
                    ca_div_end();

                    ca_text(&(Ca_TextDesc){
                        .text  = suggestions[idx].label,
                        .style = "cf-chip-label",
                    });
                }

                ca_div_end();  /* cf-cell */
            }

            ca_div_end();  /* cf-grid-row */
        }
    }

    ca_div_end();  /* cf-grid */
    ca_div_end();  /* cf-panel */
}
