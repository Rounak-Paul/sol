// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* command_flow.c — Sol's flow-based command system.
 *
 * Owns:
 *   - Key normalisation, modifier classification, key formatting.
 *   - Command-flow registry (insert / lookup by action / matching).
 *   - Leader popup open/close + suggestion collection.
 *
 * No rendering happens here; that lives in command_panel.c and status_bar.c.
 */

#include "sol_ui_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static SolCommandFlowBinding *
sol_ui_find_flow_by_action(SolUISystem *ui, const char *action)
{
    if (!ui || !action) {
        return NULL;
    }

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        if (strcmp(ui->command_flows[i].action, action) == 0) {
            return &ui->command_flows[i];
        }
    }
    return NULL;
}

static void sol_ui_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0u) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static const char *
sol_ui_flow_label_for_next(const SolUISystem *ui,
                           const SolKeyCode *prefix,
                           const SolModifierMask *prefix_modifiers,
                           size_t prefix_length,
                           SolKeyCode next_key,
                           SolModifierMask next_mods)
{
    if (!ui) {
        return "More";
    }

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, prefix, prefix_modifiers, prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= prefix_length
            || flow->sequence[prefix_length] != next_key
            || flow->step_modifiers[prefix_length] != next_mods) {
            continue;
        }
        if (flow->sequence_length == prefix_length + 1u) {
            return flow->label[0] != '\0' ? flow->label : flow->action;
        }
    }
    return "More";
}

static uint32_t
sol_ui_flow_continuation_count(const SolUISystem *ui,
                               const SolKeyCode *prefix,
                               const SolModifierMask *prefix_modifiers,
                               size_t prefix_length,
                               SolKeyCode next_key,
                               SolModifierMask next_mods)
{
    if (!ui || prefix_length + 1u >= SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        return 0u;
    }

    /* Count distinct (key,mods) pairs at the step AFTER `next`. */
    struct { SolKeyCode k; SolModifierMask m; } unique[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t     unique_count = 0u;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, prefix, prefix_modifiers, prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= prefix_length + 1u) {
            continue;
        }
        if (flow->sequence[prefix_length] != next_key
            || flow->step_modifiers[prefix_length] != next_mods) {
            continue;
        }

        const SolKeyCode      child_k = flow->sequence[prefix_length + 1u];
        const SolModifierMask child_m = flow->step_modifiers[prefix_length + 1u];
        bool exists = false;
        for (size_t j = 0u; j < unique_count; ++j) {
            if (unique[j].k == child_k && unique[j].m == child_m) {
                exists = true;
                break;
            }
        }
        if (!exists && unique_count < SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
            unique[unique_count].k = child_k;
            unique[unique_count].m = child_m;
            ++unique_count;
        }
    }
    return (uint32_t)unique_count;
}

/* ------------------------------------------------------------------ */
/* Key classification + formatting                                     */
/* ------------------------------------------------------------------ */

bool sol_ui_is_modifier_key(SolKeyCode key)
{
    switch (key) {
    case SOL_KEY_LEFT_SHIFT:
    case SOL_KEY_RIGHT_SHIFT:
    case SOL_KEY_LEFT_CTRL:
    case SOL_KEY_RIGHT_CTRL:
    case SOL_KEY_LEFT_ALT:
    case SOL_KEY_RIGHT_ALT:
    case SOL_KEY_LEFT_SUPER:
    case SOL_KEY_RIGHT_SUPER:
        return true;
    default:
        return false;
    }
}

bool sol_ui_is_leader_key(const SolUISystem *ui, SolKeyCode key)
{
    if (!ui) {
        return false;
    }
    switch (ui->leader_modifier) {
    case SOL_MOD_CTRL:  return key == SOL_KEY_LEFT_CTRL  || key == SOL_KEY_RIGHT_CTRL;
    case SOL_MOD_ALT:   return key == SOL_KEY_LEFT_ALT   || key == SOL_KEY_RIGHT_ALT;
    case SOL_MOD_SHIFT: return key == SOL_KEY_LEFT_SHIFT || key == SOL_KEY_RIGHT_SHIFT;
    case SOL_MOD_SUPER: return key == SOL_KEY_LEFT_SUPER || key == SOL_KEY_RIGHT_SUPER;
    default:            return false;
    }
}

SolKeyCode sol_ui_normalize_flow_key(SolKeyCode key)
{
    if (key >= 'a' && key <= 'z') {
        return (SolKeyCode)(key - ('a' - 'A'));
    }
    return key;
}

void sol_ui_format_key_name(SolKeyCode key, char *out, size_t out_size)
{
    if (!out || out_size == 0u) {
        return;
    }

    switch (key) {
    case SOL_KEY_LEFT_CTRL:
    case SOL_KEY_RIGHT_CTRL:
        snprintf(out, out_size, "Ctrl");  return;
    case SOL_KEY_LEFT_SHIFT:
    case SOL_KEY_RIGHT_SHIFT:
        snprintf(out, out_size, "Shift"); return;
    case SOL_KEY_LEFT_ALT:
    case SOL_KEY_RIGHT_ALT:
        snprintf(out, out_size, "Alt");   return;
    case SOL_KEY_LEFT_SUPER:
    case SOL_KEY_RIGHT_SUPER:
        snprintf(out, out_size, "Super"); return;
    case SOL_KEY_ESCAPE:
        snprintf(out, out_size, "Esc");   return;
    default: break;
    }

    /* Normalized letter keys are stored as A-Z. Display them as lowercase
       (unshifted). format_modified_key handles the shifted/uppercase case. */
    if (key >= 'A' && key <= 'Z') {
        out[0] = (char)tolower((unsigned char)key);
        out[1] = '\0';
        return;
    }
    if (key >= 'a' && key <= 'z') {
        out[0] = (char)key;
        out[1] = '\0';
        return;
    }
    if (key >= 32u && key <= 126u) {
        out[0] = (char)key;
        out[1] = '\0';
        return;
    }
    snprintf(out, out_size, "K%u", (unsigned int)key);
}

void sol_ui_format_modified_key(SolModifierMask modifiers, SolKeyCode key,
                                char *out, size_t out_size)
{
    if (!out || out_size == 0u) {
        return;
    }

    out[0] = '\0';
    size_t used = 0u;

    /* When Shift is held with a letter key, the letter becomes uppercase and
       "Shift+" is not emitted as a separate prefix — it is absorbed into the
       capitalisation.  This applies even when other modifiers (Ctrl, Alt) are
       also held, so Ctrl+Shift+n renders as "Ctrl+N" not "Ctrl+Shift+n". */
    const bool is_letter      = (key >= 'A' && key <= 'Z');
    const bool shift_held     = (modifiers & SOL_MOD_SHIFT) != 0u;
    const bool shift_absorbed = is_letter && shift_held;

    const bool skip_ctrl  = key == SOL_KEY_LEFT_CTRL  || key == SOL_KEY_RIGHT_CTRL;
    const bool skip_shift = key == SOL_KEY_LEFT_SHIFT || key == SOL_KEY_RIGHT_SHIFT
                            || shift_absorbed;
    const bool skip_alt   = key == SOL_KEY_LEFT_ALT   || key == SOL_KEY_RIGHT_ALT;
    const bool skip_super = key == SOL_KEY_LEFT_SUPER || key == SOL_KEY_RIGHT_SUPER;

    static const struct {
        SolModifierMask mask;
        const char     *prefix;
    } prefixes[] = {
        { SOL_MOD_CTRL,  "Ctrl+"  },
        { SOL_MOD_SHIFT, "Shift+" },
        { SOL_MOD_ALT,   "Alt+"   },
        { SOL_MOD_SUPER, "Super+" },
    };

    const bool skips[4] = { skip_ctrl, skip_shift, skip_alt, skip_super };

    for (size_t i = 0u; i < 4u; ++i) {
        if ((modifiers & prefixes[i].mask) == 0u || skips[i] || used >= out_size) {
            continue;
        }
        const int written = snprintf(out + used, out_size - used, "%s", prefixes[i].prefix);
        if (written > 0) {
            used += (size_t)written;
        }
    }

    if (used >= out_size) {
        out[out_size - 1u] = '\0';
        return;
    }

    /* Shift-absorbed letters: emit the uppercase key directly. */
    if (shift_absorbed) {
        if (used < out_size - 1u) {
            out[used]     = (char)key;  /* key is stored as uppercase A-Z */
            out[used + 1] = '\0';
        }
    } else {
        sol_ui_format_key_name(key, out + used, out_size - used);
    }
}

/* ------------------------------------------------------------------ */
/* Leader popup state                                                  */
/* ------------------------------------------------------------------ */

static void sol_ui_reset_leader_prefix(SolUISystem *ui)
{
    if (!ui) {
        return;
    }
    ui->leader_prefix_length    = 0u;
    ui->leader_no_match         = false;
    ui->leader_last_invalid_key = SOL_KEY_UNKNOWN;
    for (size_t i = 0u; i < SOL_UI_MAX_FLOW_SEQUENCE_LEN; ++i) {
        ui->leader_prefix[i]           = SOL_KEY_UNKNOWN;
        ui->leader_prefix_modifiers[i] = SOL_MOD_NONE;
    }
    /* The prefix changed shape — notify subscribers of the popup
       builder. Safe to bump even when the popup is closed: the popup
       builder won't be subscribed to this signal then. */
    sol_ui_bump_u32(ui->sig_leader_prefix_rev);
}

void sol_ui_open_leader_popup(SolUISystem *ui)
{
    if (!ui) {
        return;
    }
    /* Set the cached field first so non-reactive paths (key dispatch)
       observe the new state immediately, then flip the signal so
       reactive subscribers (the popup builder) re-run. */
    ui->leader_active = true;
    sol_ui_reset_leader_prefix(ui);
    ca_signal_set_bool(ui->sig_leader_active, true);
}

void sol_ui_close_leader_popup(SolUISystem *ui)
{
    if (!ui) {
        return;
    }
    ui->leader_active = false;
    sol_ui_reset_leader_prefix(ui);
    ca_signal_set_bool(ui->sig_leader_active, false);
}

/* ------------------------------------------------------------------ */
/* Flow matching                                                       */
/* ------------------------------------------------------------------ */

bool sol_ui_flow_matches_prefix(const SolCommandFlowBinding *flow,
                                const SolKeyCode *prefix,
                                const SolModifierMask *prefix_modifiers,
                                size_t prefix_length)
{
    if (!flow || prefix_length > flow->sequence_length) {
        return false;
    }
    for (size_t i = 0u; i < prefix_length; ++i) {
        if (flow->sequence[i] != prefix[i]) {
            return false;
        }
        if (prefix_modifiers && flow->step_modifiers[i] != prefix_modifiers[i]) {
            return false;
        }
    }
    return true;
}

size_t sol_ui_collect_suggestions(SolUISystem *ui,
                                  SolFlowSuggestion *out, size_t capacity)
{
    if (!ui || !out || capacity == 0u) {
        return 0u;
    }

    size_t count = 0u;
    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow,
                                        ui->leader_prefix,
                                        ui->leader_prefix_modifiers,
                                        ui->leader_prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= ui->leader_prefix_length) {
            continue;
        }

        const SolKeyCode      next_key  = flow->sequence[ui->leader_prefix_length];
        const SolModifierMask next_mods = flow->step_modifiers[ui->leader_prefix_length];

        bool exists = false;
        for (size_t j = 0u; j < count; ++j) {
            if (out[j].key == next_key && out[j].modifiers == next_mods) {
                exists = true;
                break;
            }
        }
        if (exists || count >= capacity) {
            continue;
        }

        out[count].key       = next_key;
        out[count].modifiers = next_mods;
        out[count].label     = sol_ui_flow_label_for_next(
            ui,
            ui->leader_prefix, ui->leader_prefix_modifiers,
            ui->leader_prefix_length,
            next_key, next_mods);
        out[count].continuation_count = sol_ui_flow_continuation_count(
            ui,
            ui->leader_prefix, ui->leader_prefix_modifiers,
            ui->leader_prefix_length,
            next_key, next_mods);
        ++count;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Public registration API                                             */
/* ------------------------------------------------------------------ */

bool sol_ui_system_register_command_flow(SolUISystem *ui,
                                         const SolCommandFlowDesc *desc)
{
    if (!ui || !desc || !desc->action) {
        return false;
    }
    /* `callback` is optional. When NULL, matching the flow simply
       publishes SOL_EVENT_COMMAND_INVOKED { action } and lets event
       subscribers do the work — this is how config-loaded bindings
       (which only know action names) wire up to behaviour. */

    size_t sequence_length = 0u;
    if (desc->sequence && desc->sequence_length > 0u) {
        sequence_length = desc->sequence_length;
    } else if (desc->key != SOL_KEY_UNKNOWN) {
        sequence_length = 1u;
    }
    if (sequence_length == 0u || sequence_length > SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        return false;
    }

    SolCommandFlowBinding *flow = sol_ui_find_flow_by_action(ui, desc->action);
    if (!flow) {
        if (ui->command_flow_count >= SOL_UI_MAX_COMMAND_FLOWS) {
            return false;
        }
        flow = &ui->command_flows[ui->command_flow_count++];
        memset(flow, 0, sizeof(*flow));
        sol_ui_copy_text(flow->action, sizeof(flow->action), desc->action);
    } else {
        memset(flow->sequence, 0, sizeof(flow->sequence));
    }

    flow->sequence_length = sequence_length;
    /* Default step_modifiers to 0 (no modifier) before optional copy. */
    for (size_t i = 0u; i < SOL_UI_MAX_FLOW_SEQUENCE_LEN; ++i) {
        flow->step_modifiers[i] = SOL_MOD_NONE;
    }
    if (desc->sequence && desc->sequence_length > 0u) {
        for (size_t i = 0u; i < sequence_length; ++i) {
            flow->sequence[i] = sol_ui_normalize_flow_key(desc->sequence[i]);
            if (desc->step_modifiers) {
                /* Strip leader modifier defensively — callers should
                   never include it but a stray bit must not break
                   matching. */
                flow->step_modifiers[i] =
                    (SolModifierMask)(desc->step_modifiers[i] & ~ui->leader_modifier);
            }
        }
    } else {
        flow->sequence[0] = sol_ui_normalize_flow_key(desc->key);
        if (desc->step_modifiers) {
            flow->step_modifiers[0] =
                (SolModifierMask)(desc->step_modifiers[0] & ~ui->leader_modifier);
        }
    }

    flow->callback  = desc->callback;
    flow->user_data = desc->user_data;
    sol_ui_copy_text(flow->label, sizeof(flow->label),
                     desc->label ? desc->label : desc->action);

    /* Registration affects the which-key suggestion set; notify the
       popup builder's flow-registry subscription. */
    sol_ui_bump_u32(ui->sig_flow_registry_rev);
    return true;
}
