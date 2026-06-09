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

#ifdef __cplusplus
}
#endif

#endif /* SOL_CONFIG_H */
