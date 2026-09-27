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

/*
 * Find the registered flow binding for a given action name.
 *
 * ui      The UI system to search.
 * action  The action name string to look up.
 * Returns Pointer to the matching binding, or NULL if not found.
 */
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

/*
 * Copy src into dst using snprintf, safely null-terminating the result.
 *
 * dst       Destination buffer.
 * dst_size  Size of the destination buffer in bytes.
 * src       Source string to copy (NULL clears dst).
 */
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

/*
 * Return true when two flow bindings share an identical key-sequence chord.
 *
 * a  First binding to compare.
 * b  Second binding to compare.
 */
static bool sol_ui_flows_have_same_chord(const SolCommandFlowBinding *a,
                                         const SolCommandFlowBinding *b)
{
    if (!a || !b || a->sequence_length != b->sequence_length) return false;
    for (size_t i = 0u; i < a->sequence_length; ++i) {
        if (a->sequence[i] != b->sequence[i] ||
            a->step_modifiers[i] != b->step_modifiers[i]) {
            return false;
        }
    }
    return true;
}

/*
 * Remove the flow binding at the given index, shifting later entries down.
 *
 * ui     The UI system owning the bindings array.
 * index  Zero-based index of the binding to remove.
 */
static void sol_ui_remove_flow_at(SolUISystem *ui, size_t index)
{
    if (!ui || index >= ui->command_flow_count) return;
    for (size_t i = index + 1u; i < ui->command_flow_count; ++i) {
        ui->command_flows[i - 1u] = ui->command_flows[i];
    }
    ui->command_flow_count--;
    memset(&ui->command_flows[ui->command_flow_count], 0,
           sizeof(ui->command_flows[ui->command_flow_count]));
}

/*
 * Return the display label for the flow that would be completed by pressing
 * next_key/next_mods after the given prefix.
 *
 * ui               The UI system to search.
 * prefix           Key codes of the already-pressed prefix steps.
 * prefix_modifiers Modifier masks for each prefix step.
 * prefix_length    Number of steps in the prefix.
 * next_key         The candidate next key code.
 * next_mods        Modifier mask for the candidate next key.
 * Returns          The flow's label (or action name), or "More" when the
 *                  candidate extends into a deeper sub-sequence.
 */
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

/*
 * Count the number of distinct key continuations available one step beyond
 * the given prefix + next_key combination.
 *
 * ui               The UI system to search.
 * prefix           Key codes of the already-pressed prefix steps.
 * prefix_modifiers Modifier masks for each prefix step.
 * prefix_length    Number of steps in the prefix.
 * next_key         The candidate next key code.
 * next_mods        Modifier mask for the candidate next key.
 * Returns          Number of distinct (key, modifiers) pairs that follow.
 */
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

/*
 * Return true when key is one of the eight physical modifier keys.
 *
 * key     Key code to test.
 * Returns true if key is Ctrl, Shift, Alt, or Super (either side).
 */
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

/*
 * Return true when key matches the physical key(s) bound to the leader modifier.
 *
 * ui   The UI system whose leader_modifier is used for the comparison.
 * key  Key code to test.
 */
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

/*
 * Normalise a key code so that lowercase letters are stored as uppercase.
 *
 * key     The raw key code to normalise.
 * Returns The normalised key code.
 */
SolKeyCode sol_ui_normalize_flow_key(SolKeyCode key)
{
    if (key >= 'a' && key <= 'z') {
        return (SolKeyCode)(key - ('a' - 'A'));
    }
    return key;
}

/*
 * Write a human-readable name for a single key code into out.
 *
 * key       The key code to format.
 * out       Destination buffer.
 * out_size  Size of the destination buffer in bytes.
 */
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

/*
 * Format a key chord (modifiers + key) as a human-readable string such as
 * "Ctrl+N" or "Alt+Shift+f".  Shift is absorbed into letter capitalisation
 * rather than emitted as an explicit "Shift+" prefix.
 *
 * modifiers  The modifier mask held at the time the key was pressed.
 * key        The (normalised) key code.
 * out        Destination buffer.
 * out_size   Size of the destination buffer in bytes.
 */
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

/*
 * Clear the leader prefix buffer and reset all associated state fields,
 * then bump sig_leader_prefix_rev to notify popup-builder subscribers.
 *
 * ui  The UI system whose leader state should be reset.
 */
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

/*
 * Activate the leader popup, resetting the prefix buffer and signalling
 * reactive subscribers to rebuild the popup content.
 *
 * ui  The UI system on which to open the popup.
 */
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

/*
 * Deactivate the leader popup, clearing all prefix state and notifying
 * reactive subscribers.
 *
 * ui  The UI system on which to close the popup.
 */
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

/*
 * Return true when a flow binding's sequence starts with the given prefix.
 *
 * flow             The flow binding to test.
 * prefix           Array of key codes forming the prefix to match.
 * prefix_modifiers Modifier mask array for each prefix step (may be NULL to
 *                  skip modifier comparison).
 * prefix_length    Number of steps in the prefix.
 */
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

/*
 * Populate out with the unique next-step suggestions reachable from the
 * current leader prefix, for use by the which-key popup.
 *
 * ui        The UI system providing the flow registry and current prefix.
 * out       Output array of SolFlowSuggestion to fill.
 * capacity  Maximum number of suggestions to write into out.
 * Returns   Number of suggestions written.
 */
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
/* Chord assignment                                                    */
/* ------------------------------------------------------------------ */

/*
 * Give a flow its effective chord, resolving collisions so every chord has
 * at most one owner. A chord from the user keymap always wins; an owner
 * default never displaces a user chord (so plugin load order cannot undo
 * bindings.conf); between two defaults the later assignment wins. A
 * displaced flow stays registered, just unbound.
 *
 * ui             UI system owning the registry.
 * flow           Flow to update.
 * sequence       Key steps (ignored when length is 0).
 * modifiers      Per-step modifiers, or NULL for none.
 * length         Step count; 0 unbinds the flow.
 * from_override  true when the chord comes from the user keymap.
 */
static void sol_ui_flow_assign_chord(SolUISystem           *ui,
                                     SolCommandFlowBinding *flow,
                                     const SolKeyCode      *sequence,
                                     const SolModifierMask *modifiers,
                                     size_t                 length,
                                     bool                   from_override)
{
    memset(flow->sequence, 0, sizeof(flow->sequence));
    memset(flow->step_modifiers, 0, sizeof(flow->step_modifiers));
    flow->sequence_length = 0u;
    flow->user_chord      = from_override;
    if (length == 0u || !sequence) return;

    SolCommandFlowBinding candidate = *flow;
    for (size_t i = 0u; i < length; ++i) {
        candidate.sequence[i] = sol_ui_normalize_flow_key(sequence[i]);
        candidate.step_modifiers[i] = modifiers
            ? (SolModifierMask)(modifiers[i] & ~ui->leader_modifier)
            : SOL_MOD_NONE;
    }
    candidate.sequence_length = length;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *other = &ui->command_flows[i];
        if (other == flow || other->sequence_length == 0u) continue;
        if (!sol_ui_flows_have_same_chord(other, &candidate)) continue;
        if (other->user_chord && !from_override) return;
        memset(other->sequence, 0, sizeof(other->sequence));
        memset(other->step_modifiers, 0, sizeof(other->step_modifiers));
        other->sequence_length = 0u;
        other->user_chord      = false;
    }

    memcpy(flow->sequence, candidate.sequence, sizeof(flow->sequence));
    memcpy(flow->step_modifiers, candidate.step_modifiers, sizeof(flow->step_modifiers));
    flow->sequence_length = length;
}

/* Return the keymap override for action, or NULL. */
static SolKeymapOverride *sol_ui_find_override(SolUISystem *ui, const char *action)
{
    for (size_t i = 0u; i < ui->keymap_override_count; ++i) {
        if (strcmp(ui->keymap_overrides[i].action, action) == 0) {
            return &ui->keymap_overrides[i];
        }
    }
    return NULL;
}

/* Append an empty flow for action; NULL when the registry is full. */
static SolCommandFlowBinding *sol_ui_append_flow(SolUISystem *ui, const char *action)
{
    if (ui->command_flow_count >= SOL_UI_MAX_COMMAND_FLOWS) return NULL;
    SolCommandFlowBinding *flow = &ui->command_flows[ui->command_flow_count++];
    memset(flow, 0, sizeof(*flow));
    sol_ui_copy_text(flow->action, sizeof(flow->action), action);
    sol_ui_copy_text(flow->label, sizeof(flow->label), action);
    return flow;
}

/* ------------------------------------------------------------------ */
/* Public registration API                                             */
/* ------------------------------------------------------------------ */

/*
 * Register a command, or update the one with the same action name. The
 * descriptor's chord becomes the command's default; the effective chord is
 * the user's keymap override when one exists. A descriptor without a chord
 * registers an unbound (menu/palette-only) command.
 *
 * ui    The UI system to register into.
 * desc  Action name, label, optional chord, and optional callback. A NULL
 *       callback means matching only publishes SOL_EVENT_COMMAND_INVOKED.
 * Returns true on success, false when the descriptor is invalid or the
 *         registry is full.
 */
bool sol_ui_system_register_command_flow(SolUISystem *ui,
                                         const SolCommandFlowDesc *desc)
{
    if (!ui || !desc || !desc->action || !desc->action[0]) {
        return false;
    }

    SolKeyCode      default_seq[SOL_UI_MAX_FLOW_SEQUENCE_LEN] = {0};
    SolModifierMask default_mods[SOL_UI_MAX_FLOW_SEQUENCE_LEN] = {0};
    size_t          default_len = 0u;
    if (desc->sequence && desc->sequence_length > 0u) {
        if (desc->sequence_length > SOL_UI_MAX_FLOW_SEQUENCE_LEN) return false;
        default_len = desc->sequence_length;
        memcpy(default_seq, desc->sequence, default_len * sizeof(default_seq[0]));
        if (desc->step_modifiers) {
            memcpy(default_mods, desc->step_modifiers, default_len * sizeof(default_mods[0]));
        }
    } else if (desc->key != SOL_KEY_UNKNOWN) {
        default_len     = 1u;
        default_seq[0]  = desc->key;
        default_mods[0] = desc->step_modifiers ? desc->step_modifiers[0] : SOL_MOD_NONE;
    }
    for (size_t i = 0u; i < default_len; ++i) {
        default_seq[i]  = sol_ui_normalize_flow_key(default_seq[i]);
        default_mods[i] = (SolModifierMask)(default_mods[i] & ~ui->leader_modifier);
    }

    SolCommandFlowBinding *flow = sol_ui_find_flow_by_action(ui, desc->action);
    if (!flow) flow = sol_ui_append_flow(ui, desc->action);
    if (!flow) return false;

    flow->owned     = true;
    flow->callback  = desc->callback;
    flow->user_data = desc->user_data;
    sol_ui_copy_text(flow->label, sizeof(flow->label),
                     desc->label ? desc->label : desc->action);
    memcpy(flow->default_sequence, default_seq, sizeof(flow->default_sequence));
    memcpy(flow->default_modifiers, default_mods, sizeof(flow->default_modifiers));
    flow->default_length = default_len;

    const SolKeymapOverride *ov = sol_ui_find_override(ui, desc->action);
    if (ov) {
        sol_ui_flow_assign_chord(ui, flow, ov->sequence, ov->modifiers, ov->length, true);
    } else {
        sol_ui_flow_assign_chord(ui, flow, default_seq, default_mods, default_len, false);
    }

    /* Registration affects the which-key suggestion set; notify the
       popup builder's flow-registry subscription. */
    sol_ui_bump_u32(ui->sig_flow_registry_rev);
    return true;
}

/*
 * Set the user's chord for an action (the bindings.conf layer). It applies
 * to the command now if registered, and to its registration later if not.
 * An action nobody has registered becomes an event-driven command.
 *
 * ui         The UI system.
 * action     Action name.
 * sequence   Key steps after the leader; NULL or length 0 unbinds.
 * modifiers  Per-step modifiers (may be NULL).
 * length     Step count.
 * Returns    false for invalid input or when the keymap/registry is full.
 */
bool sol_ui_system_set_keymap_override(SolUISystem           *ui,
                                       const char            *action,
                                       const SolKeyCode      *sequence,
                                       const SolModifierMask *modifiers,
                                       size_t                 length)
{
    if (!ui || !action || !action[0] || length > SOL_UI_MAX_FLOW_SEQUENCE_LEN) return false;
    if (!sequence) length = 0u;

    SolKeymapOverride *ov = sol_ui_find_override(ui, action);
    if (!ov) {
        if (ui->keymap_override_count >= SOL_UI_MAX_COMMAND_FLOWS) return false;
        ov = &ui->keymap_overrides[ui->keymap_override_count++];
        memset(ov, 0, sizeof(*ov));
        sol_ui_copy_text(ov->action, sizeof(ov->action), action);
    }
    memset(ov->sequence, 0, sizeof(ov->sequence));
    memset(ov->modifiers, 0, sizeof(ov->modifiers));
    ov->length = length;
    for (size_t i = 0u; i < length; ++i) {
        ov->sequence[i]  = sequence[i];
        ov->modifiers[i] = modifiers ? modifiers[i] : SOL_MOD_NONE;
    }

    SolCommandFlowBinding *flow = sol_ui_find_flow_by_action(ui, action);
    if (!flow && length > 0u) flow = sol_ui_append_flow(ui, action);
    if (flow) sol_ui_flow_assign_chord(ui, flow, ov->sequence, ov->modifiers, ov->length, true);
    else if (length > 0u) return false;

    sol_ui_bump_u32(ui->sig_flow_registry_rev);
    return true;
}

/*
 * Drop the user keymap: forget every override, remove commands that only
 * existed because of one, and give every registered command its default
 * chord again (in registration order, as at startup). Any chord in
 * progress is cancelled since its prefix may no longer lead anywhere.
 *
 * ui  The UI system.
 */
void sol_ui_system_reset_keymap(SolUISystem *ui)
{
    if (!ui) return;
    memset(ui->keymap_overrides, 0, sizeof(ui->keymap_overrides));
    ui->keymap_override_count = 0u;

    for (size_t i = ui->command_flow_count; i > 0u; --i) {
        if (!ui->command_flows[i - 1u].owned) sol_ui_remove_flow_at(ui, i - 1u);
    }
    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *flow = &ui->command_flows[i];
        flow->sequence_length = 0u;
        flow->user_chord      = false;
    }
    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *flow = &ui->command_flows[i];
        sol_ui_flow_assign_chord(ui, flow, flow->default_sequence,
                                 flow->default_modifiers, flow->default_length, false);
    }

    sol_ui_reset_leader_prefix(ui);
    sol_ui_bump_u32(ui->sig_flow_registry_rev);
}

/*
 * Remove the flow binding for the given action name from the registry.
 * The user's keymap override (if any) is kept so a re-registration of the
 * same action picks it up again.
 *
 * ui      The UI system to modify.
 * action  The action name whose binding should be removed.
 * Returns true if the binding was found and removed, false otherwise.
 */
bool sol_ui_system_unregister_command_flow(SolUISystem *ui, const char *action)
{
    if (!ui || !action) {
        return false;
    }

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        if (strcmp(ui->command_flows[i].action, action) != 0) {
            continue;
        }
        sol_ui_remove_flow_at(ui, i);
        sol_ui_bump_u32(ui->sig_flow_registry_rev);
        return true;
    }
    return false;
}
