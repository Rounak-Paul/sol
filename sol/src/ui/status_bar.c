// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* status_bar.c — Sol's status bar rendering + status text helpers. */

#include "sol_ui_internal.h"

#include <stdio.h>
#include <string.h>

/*
 * Set the status-bar message, avoiding a redundant invalidation when the
 * kind and text are unchanged.
 *
 * ui    The UI system whose status bar is updated.
 * kind  Badge kind character (SOL_UI_STATUS_KIND_*).
 * text  Message string to display (NULL clears the bar).
 */
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

/*
 * Format a single key chord and display it in the status bar with the KEY
 * badge kind.
 *
 * ui         The UI system whose status bar is updated.
 * key        The key code to format.
 * modifiers  Active modifier mask.
 */
void sol_ui_set_status_key(SolUISystem *ui, SolKeyCode key, SolModifierMask modifiers)
{
    char buf[48];
    sol_ui_format_modified_key(modifiers, key, buf, sizeof(buf));
    sol_ui_set_status_text(ui, SOL_UI_STATUS_KIND_KEY, buf);
}

/*
 * Format a multi-step key sequence as space-separated key names and display
 * it in the status bar with the COMMAND badge kind.
 *
 * ui              The UI system whose status bar is updated.
 * sequence        Array of key codes for each sequence step.
 * length          Number of steps in the sequence.
 * step_modifiers  Per-step modifier masks (may be NULL; used for intermediate
 *                 steps only).
 * last_modifiers  Live modifier mask applied to the final step.
 */
void sol_ui_set_status_sequence(SolUISystem *ui,
                                const SolKeyCode *sequence, size_t length,
                                const SolModifierMask *step_modifiers,
                                SolModifierMask last_modifiers)
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
            /* Last step: use the live modifier mask (includes leader mod). */
            sol_ui_format_modified_key(last_modifiers, sequence[i], part, sizeof(part));
        } else if (step_modifiers && step_modifiers[i] != SOL_MOD_NONE) {
            /* Intermediate step with stored per-step modifiers. */
            sol_ui_format_modified_key(step_modifiers[i], sequence[i], part, sizeof(part));
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

/*
 * Map a status-bar kind character to its CSS badge style class.
 *
 * kind    SOL_UI_STATUS_KIND_* character.
 * Returns The CSS class string for the badge div.
 */
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

/*
 * Emit the Causality nodes for the entire status bar: the left section with
 * the optional kind badge and message text, and the right section with any
 * active plugin status segments.
 *
 * ui  The UI system providing status text and plugin segment data.
 */
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

    /* Right side: plugin status segments */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "status-bar-right",
    });
    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i) {
        if (!ui->plugin_status_segs[i].in_use) continue;
        const char *cls = ui->plugin_status_segs[i].style_class[0]
            ? ui->plugin_status_segs[i].style_class
            : "status-plugin-segment";
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = cls,
        });
        ca_text(&(Ca_TextDesc){ .text = ui->plugin_status_segs[i].text });
        ca_div_end();
    }
    ca_div_end();  /* status-bar-right */

    ca_div_end();  /* status-bar */
}

/* ================================================================== */
/* Plugin status segment API                                           */
/* ================================================================== */

/*
 * Allocate a new plugin status segment with the given text and optional CSS
 * style class, and invalidate the status bar.
 *
 * ui           The UI system owning the segment array.
 * text         Initial display text for the segment.
 * style_class  Optional CSS class (NULL uses the default segment style).
 * Returns      A unique SolUIStatusToken to identify this segment, or
 *              SOL_UI_STATUS_TOKEN_INVALID when the array is full.
 */
SolUIStatusToken sol_ui_system_add_status_segment(SolUISystem *ui,
                                                   const char  *text,
                                                   const char  *style_class)
{
    if (!ui) return SOL_UI_STATUS_TOKEN_INVALID;

    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i) {
        if (ui->plugin_status_segs[i].in_use) continue;

        SolUIStatusSegment *seg = &ui->plugin_status_segs[i];
        if (ui->plugin_status_next_token == 0u)
            ui->plugin_status_next_token = 1u;
        seg->token  = ui->plugin_status_next_token++;
        seg->in_use = true;
        snprintf(seg->text,        sizeof(seg->text),
                 "%s", text        ? text        : "");
        snprintf(seg->style_class, sizeof(seg->style_class),
                 "%s", style_class ? style_class : "");

        if (ui->primary_window)
            ca_window_invalidate_status_bar(ui->primary_window);
        return seg->token;
    }

    return SOL_UI_STATUS_TOKEN_INVALID;
}

/*
 * Update the display text of an existing plugin status segment and
 * invalidate the status bar.
 *
 * ui     The UI system owning the segment array.
 * token  Token returned by sol_ui_system_add_status_segment.
 * text   New display text (NULL clears the segment text).
 */
void sol_ui_system_update_status_segment(SolUISystem     *ui,
                                          SolUIStatusToken token,
                                          const char      *text)
{
    if (!ui || token == SOL_UI_STATUS_TOKEN_INVALID) return;

    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i) {
        if (!ui->plugin_status_segs[i].in_use) continue;
        if (ui->plugin_status_segs[i].token != token) continue;

        snprintf(ui->plugin_status_segs[i].text,
                 sizeof(ui->plugin_status_segs[i].text),
                 "%s", text ? text : "");
        if (ui->primary_window)
            ca_window_invalidate_status_bar(ui->primary_window);
        return;
    }
}

/*
 * Remove a plugin status segment by token and invalidate the status bar.
 *
 * ui     The UI system owning the segment array.
 * token  Token of the segment to remove.
 */
void sol_ui_system_remove_status_segment(SolUISystem     *ui,
                                          SolUIStatusToken token)
{
    if (!ui || token == SOL_UI_STATUS_TOKEN_INVALID) return;

    for (uint32_t i = 0u; i < SOL_UI_MAX_STATUS_SEGMENTS; ++i) {
        if (!ui->plugin_status_segs[i].in_use) continue;
        if (ui->plugin_status_segs[i].token != token) continue;

        memset(&ui->plugin_status_segs[i], 0, sizeof(ui->plugin_status_segs[i]));
        if (ui->primary_window)
            ca_window_invalidate_status_bar(ui->primary_window);
        return;
    }
}
