// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_file_tree.h — Directory hierarchy state for Sol's left-side
 * file panel.
 *
 * The tree owns a single root directory and a flat list of "visible"
 * entries (the depth-first projection of the expansion state). UI code
 * iterates this list to render rows; expanding/collapsing a directory
 * inserts/removes its children in the same vector so subsequent draws
 * stay O(visible).
 *
 * Boundaries:
 *   - No causality, no buffer-system, no UI dependencies. Pure state +
 *     platform-abstracted directory scan.
 *   - Reusable: the same module can back a tree in the editor's left
 *     pane today and a future file-picker window tomorrow.
 */

#ifndef SOL_FILE_TREE_H
#define SOL_FILE_TREE_H

#include <stdbool.h>
#include <stddef.h>

/* Forward-declaration only — the file tree does not link against
 * causality; it just bumps a caller-owned u32 signal on every
 * mutation when one is attached. */
typedef struct Ca_Signal Ca_Signal;

/* Forward-decl of the event bus — see sol_event.h. When attached
 * the tree publishes sol.file_tree.root_changed on set_root. */
typedef struct SolEventBus SolEventBus;

typedef struct SolFileTree  SolFileTree;
typedef struct SolFileEntry SolFileEntry;

/* A single row in the visible projection. The pointers are owned by
 * the tree and remain valid until the next mutation (set_root,
 * toggle, refresh). */
struct SolFileEntry {
    const char *name;       /* basename (no slash)            */
    const char *full_path;  /* absolute path                  */
    size_t      depth;      /* 0 = directly under root        */
    bool        is_dir;     /* true for directories           */
    bool        expanded;   /* meaningful when is_dir         */
};

SolFileTree *sol_file_tree_create(void);
void         sol_file_tree_destroy(SolFileTree *tree);

/* Attach (or detach with NULL) a u32 revision signal that the tree
 * bumps on every mutation (set_root, toggle, refresh). Same contract
 * as sol_buffer_attach_revision_signal. */
void sol_file_tree_attach_revision_signal(SolFileTree *tree, Ca_Signal *sig);

/* Attach (or detach with NULL) an event bus. When attached, the tree
 * publishes sol.file_tree.root_changed on every set_root success. */
void sol_file_tree_attach_event_bus(SolFileTree *tree, SolEventBus *bus);

/* Set (or replace) the root directory. Returns false if the path is
 * not a directory or could not be read; in that case the tree is
 * cleared. */
bool         sol_file_tree_set_root(SolFileTree *tree, const char *root_path);

/* Returns NULL if no root is set. */
const char  *sol_file_tree_root(const SolFileTree *tree);

/* Visible projection. */
size_t                sol_file_tree_visible_count(const SolFileTree *tree);
const SolFileEntry   *sol_file_tree_visible(const SolFileTree *tree, size_t index);

/* Toggle expansion on a directory row. No-op for files or invalid
 * indices. Returns true if the visible list changed. */
bool sol_file_tree_toggle(SolFileTree *tree, size_t index);

/* Re-read the mounted root after external filesystem mutations.
 * Expansion state is rebuilt from the root; callers that need stable
 * expansion should add that policy above this pure tree layer. */
bool sol_file_tree_refresh(SolFileTree *tree);

#endif /* SOL_FILE_TREE_H */
