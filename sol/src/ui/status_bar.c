// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* status_bar.c — Sol's status bar rendering + status text helpers. */

#include "sol_ui_internal.h"

#include <stdio.h>
#include <string.h>

void sol_ui_set_status_text(SolUISystem *ui, char kind, const char *text)
{
    if (!ui) {
        return;
    }

    char next[SOL_UI_STATUS_TEXT_MAX_LEN + 1u];
    next[0] = '\0';
    if (text) {
        snprintf(next, sizeof(next), "%s", text);
    }

    if (ui->status_bar_kind == kind && strcmp(ui->status_bar_text, next) == 0) {
        return;
    }

    ui->status_bar_kind = kind;
    snprintf(ui->status_bar_text, sizeof(ui->status_bar_text), "%s", next);

    /* Status bar lives in causality's system-managed strip, not in the
       reactive workspace tree, so invalidate it directly. */
    if (ui->primary_window) {
        ca_window_invalidate_status_bar(ui->primary_window);
    }
}

void sol_ui_set_status_key(SolUISystem *ui, SolKeyCode key, SolModifierMask modifiers)
{
    char buf[48];
    sol_ui_format_modified_key(modifiers, key, buf, sizeof(buf));
    sol_ui_set_status_text(ui, SOL_UI_STATUS_KIND_KEY, buf);
}

void sol_ui_set_status_sequence(SolUISystem *ui,
                                const SolKeyCode *sequence, size_t length,
                                SolModifierMask modifiers)
{
    if (!ui || !sequence || length == 0u) {
        return;
    }

    char   text[SOL_UI_STATUS_TEXT_MAX_LEN + 1u];
    size_t used = 0u;
    text[0] = '\0';

    for (size_t i = 0u; i < length; ++i) {
        char part[48];
        if (i + 1u == length) {
            sol_ui_format_modified_key(modifiers, sequence[i], part, sizeof(part));
        } else {
            sol_ui_format_key_name(sequence[i], part, sizeof(part));
        }
        const int written = snprintf(text + used, sizeof(text) - used,
                                     "%s%s", i == 0u ? "" : " ", part);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
        if (used >= sizeof(text)) {
            text[sizeof(text) - 1u] = '\0';
            break;
        }
    }

    sol_ui_set_status_text(ui, SOL_UI_STATUS_KIND_COMMAND, text);
}

static const char *sol_ui_status_badge_style(char kind)
{
    switch (kind) {
    case SOL_UI_STATUS_KIND_COMMAND:
        return "status-bar-badge status-bar-badge-command";
    case SOL_UI_STATUS_KIND_LEADER:
        return "status-bar-badge status-bar-badge-leader";
    default:
        return "status-bar-badge status-bar-badge-key";
    }
}

void sol_ui_render_status_bar(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    /* Causality's status_bar_node is the outer horizontal strip of the
       configured height; this wrapper only carries the "status-bar"
       style class and inherits the parent's full height (height = 0
       means "auto-fill" in the cross axis of a horizontal container). */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "status-bar",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "status-bar-left",
    });

    if (ui->status_bar_text[0] != '\0') {
        const char badge_text[2] = { ui->status_bar_kind, '\0' };

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = sol_ui_status_badge_style(ui->status_bar_kind),
        });
        ca_text(&(Ca_TextDesc){ .text = badge_text, .style = "status-bar-badge-text" });
        ca_div_end();

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "status-bar-value",
        });
        ca_text(&(Ca_TextDesc){ .text = ui->status_bar_text, .style = "status-bar-text" });
        ca_div_end();
    }

    ca_div_end();  /* status-bar-left */
    ca_div_end();  /* status-bar */
}
