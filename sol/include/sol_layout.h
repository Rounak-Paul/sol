// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_layout.h — Durable workspace arrangement state.
 *
 * Remembers how the user last arranged the workspace (sidebar width,
 * terminal dock position and size, search-window split) so every new
 * project and every launch opens with the same arrangement.
 *
 * Stored in $HOME/.sol/layout as one "key value" pair per line:
 *
 *   tree_ratio 0.2000
 *   terminal_position bottom
 *   terminal_ratio 0.3000
 *   search_ratio 0.3600
 *
 * This is session state, not a preference: it is kept out of
 * settings.json so dragging a splitter never rewrites user preferences or
 * triggers the cross-instance settings reload. Unknown keys, malformed
 * lines and out-of-range values fall back to their defaults.
 */

#ifndef SOL_LAYOUT_H
#define SOL_LAYOUT_H

#include "sol_terminal.h"

#include <stdbool.h>

/* Sidebar (file tree / plugin panel) share of the workspace width. */
#define SOL_LAYOUT_TREE_RATIO_MIN          0.10f
#define SOL_LAYOUT_TREE_RATIO_MAX          0.50f
#define SOL_LAYOUT_TREE_RATIO_DEFAULT      0.20f

/* Docked terminal share of the buffer/terminal split axis. */
#define SOL_LAYOUT_TERMINAL_RATIO_MIN      0.20f
#define SOL_LAYOUT_TERMINAL_RATIO_MAX      0.80f
#define SOL_LAYOUT_TERMINAL_RATIO_DEFAULT  0.30f

/* Search-window results list share of the results/preview split. */
#define SOL_LAYOUT_SEARCH_RATIO_MIN        0.24f
#define SOL_LAYOUT_SEARCH_RATIO_MAX        0.62f
#define SOL_LAYOUT_SEARCH_RATIO_DEFAULT    0.36f

/*
 * Workspace arrangement persisted across launches.
 */
typedef struct SolLayout {
    /* Sidebar width as a fraction of the workspace width. */
    float tree_ratio;

    /* Where the terminal panel docks (bottom, right, or floating). */
    SolTerminalPosition terminal_position;

    /* Terminal panel fraction of the docked split axis; shared by the
       bottom and right positions. */
    float terminal_ratio;

    /* Search-window results list fraction of the results/preview split. */
    float search_ratio;
} SolLayout;

/* Return a SolLayout with every field set to its documented default. */
SolLayout sol_layout_defaults(void);

/*
 * Clamp every field of layout into its valid range, replacing non-finite
 * ratios and unknown terminal positions with defaults.
 *
 * layout  Layout to sanitize in place; NULL is ignored.
 */
void sol_layout_sanitize(SolLayout *layout);

/*
 * Load the layout from $HOME/.sol/layout.
 *
 * out is always filled: fields missing or invalid in the file keep their
 * defaults, and an absent or unreadable file yields sol_layout_defaults().
 *
 * out      Receives the loaded layout.
 * Returns  true when the file was read, false when defaults were used.
 */
bool sol_layout_load(SolLayout *out);

/*
 * Atomically write layout to $HOME/.sol/layout.
 *
 * The data is written to a per-process temp file, synced, then renamed
 * over the destination so readers never observe a torn file.
 *
 * layout   Layout to persist (sanitized before writing).
 * Returns  true on success.
 */
bool sol_layout_save(const SolLayout *layout);

#endif /* SOL_LAYOUT_H */
