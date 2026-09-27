// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_config.h — Per-user configuration directory + bindings loader.
 *
 * Sol stores user-overridable configuration under a single directory:
 *
 *     $HOME/.sol/                    (POSIX)
 *     %APPDATA%/sol/                 (Win32)
 *
 * Today the only file consumed from there is `bindings.conf`, which
 * declares leader-chord → action mappings. The format is intentionally
 * line-oriented so it's friendly to both hand-edits and plugins that
 * generate snippets:
 *
 *     # Comment lines start with '#'.
 *     leader <ctrl|alt|super|shift>
 *     bind L <chord> <action>
 *     unbind <action>
 *
 * <chord> is a whitespace-separated sequence of keys. Each key may be
 * prefixed by `shift+`, `alt+`, `super+` (case-insensitive). The token
 * after `bind` is `L`, the leader placeholder. Examples:
 *
 *     bind L b b            buffer.focus.previous
 *     bind L b n            buffer.cycle.next
 *     bind L b c            buffer.new
 *     bind L p v            pane.split.vertical
 *
 * <action> is a dotted string published as the payload of
 * SOL_EVENT_COMMAND_INVOKED when the chord fires. Any subscriber on
 * the event bus can react to it — this is the extensibility seam.
 */

#ifndef SOL_CONFIG_H
#define SOL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "sol_ui_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve the per-user config directory, creating it on first use.
 *
 * Returns  A freshly-allocated absolute path the caller must free(),
 *          or NULL on failure (HOME unset, mkdir failed, etc.).
 */
char *sol_config_dir(void);

/*
 * Compose the absolute path to a file inside the config directory.
 *
 * filename  Name of the file relative to the config directory.
 * Returns   A freshly-allocated path the caller must free(), or NULL on failure.
 */
char *sol_config_path(const char *filename);

/*
 * Load bindings.conf and register each binding on the UI system.
 *
 * Writes a default bindings.conf with Sol's built-in bindings when the
 * file is missing. Per-line parse errors are reported on stderr and
 * skipped without aborting the load.
 *
 * ui       The UI system to register bindings on.
 * Returns  Number of bindings successfully registered, or -1 on fatal I/O error.
 */
int sol_config_load_bindings(SolUISystem *ui);

/*
 * Parse leader-relative chord text such as "b shift+s" into key steps.
 *
 * text        Whitespace-separated step tokens (leader implicit).
 * leader      Active leader modifier; steps using it or ctrl are rejected.
 * sequence    Receives the key code of each step.
 * modifiers   Receives the non-leader modifier mask of each step.
 * out_length  Receives the step count.
 * Returns     false when the text is not a valid chord.
 */
bool sol_config_parse_chord(const char      *text,
                            SolModifierMask  leader,
                            SolKeyCode       sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN],
                            SolModifierMask  modifiers[SOL_UI_MAX_FLOW_SEQUENCE_LEN],
                            size_t          *out_length);

/*
 * Format key steps as chord text accepted by sol_config_parse_chord.
 *
 * sequence   Key code per step.
 * modifiers  Modifier mask per step (NULL for none).
 * length     Number of steps (at least 1).
 * buf        Destination buffer.
 * size       Capacity of buf in bytes.
 * Returns    false when a key has no textual name or buf is too small.
 */
bool sol_config_format_chord(const SolKeyCode      *sequence,
                             const SolModifierMask *modifiers,
                             size_t                 length,
                             char                  *buf,
                             size_t                 size);

/*
 * Canonical lowercase name of a leader modifier.
 *
 * mod      SOL_MOD_CTRL, SOL_MOD_ALT, SOL_MOD_SUPER or SOL_MOD_SHIFT.
 * Returns  "ctrl", "alt", "super" or "shift" ("ctrl" for anything else).
 */
const char *sol_config_modifier_name(SolModifierMask mod);

/*
 * Persist one action's chord in bindings.conf, replacing its existing
 * bind/unbind line and leaving every other line untouched. The write is
 * atomic. A NULL sequence or zero length writes `unbind <action>`.
 *
 * action     Action name.
 * sequence   Key steps after the leader.
 * modifiers  Per-step modifier masks (may be NULL).
 * length     Step count.
 * Returns    true on success.
 */
bool sol_config_save_binding(const char            *action,
                             const SolKeyCode      *sequence,
                             const SolModifierMask *modifiers,
                             size_t                 length);

/*
 * Persist the leader modifier in bindings.conf (atomic). Bind lines that
 * spell the previous leader literally are rewritten to `L`.
 *
 * previous  Leader before the change.
 * leader    New leader.
 * Returns   true on success.
 */
bool sol_config_save_leader(SolModifierMask previous, SolModifierMask leader);

/* One action and its leader-relative chord text, e.g. "b shift+s". */
typedef struct SolConfigBinding {
    char action[64];
    char chord[64];
} SolConfigBinding;

/*
 * Enumerate Sol's default bindings (the template written on first launch).
 *
 * out       Receives up to capacity entries; NULL only counts.
 * capacity  Capacity of out.
 * Returns   Total number of default bindings, which may exceed capacity.
 */
size_t sol_config_default_bindings(SolConfigBinding *out, size_t capacity);

/*
 * Overwrite bindings.conf with Sol's default template (atomic).
 *
 * Returns true on success.
 */
bool sol_config_reset_bindings(void);

#ifdef __cplusplus
}
#endif

#endif /* SOL_CONFIG_H */
