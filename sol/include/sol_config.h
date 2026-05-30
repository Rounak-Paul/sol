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
 *     bind <chord> <action>
 *
 * <chord> is a whitespace-separated sequence of keys. Each key may be
 * prefixed by `shift+`, `alt+`, `super+` (case-insensitive). The first
 * token MUST be `ctrl` (the leader). Examples:
 *
 *     bind ctrl b b            buffer.focus.previous
 *     bind ctrl b n            buffer.cycle.next
 *     bind ctrl b c            buffer.new
 *     bind ctrl p v            pane.split.vertical
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

/* Resolve the per-user config directory, creating it (and any missing
 * parents) on first use. Returns a freshly-allocated absolute path the
 * caller must free(); NULL on failure (HOME unset, mkdir failed, …). */
char *sol_config_dir(void);

/* Compose `<config_dir>/<filename>`. Ensures the directory exists.
 * Caller frees. NULL on failure. */
char *sol_config_path(const char *filename);

/* Load bindings.conf, registering each binding on `ui` via
 * `sol_ui_system_register_command_flow`. Writes a default file with
 * Sol's built-in bindings when the file is missing.
 *
 * Returns the number of bindings successfully registered, or -1 on a
 * fatal I/O error. Per-line parse errors are reported on stderr and
 * skipped — they do not abort loading. */
int sol_config_load_bindings(SolUISystem *ui);

#ifdef __cplusplus
}
#endif

#endif /* SOL_CONFIG_H */
