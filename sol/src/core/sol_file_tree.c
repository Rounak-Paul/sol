// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_file_tree.c — Implementation of sol_file_tree.h.
 *
 * The tree keeps a flat heap-allocated array of nodes (`Node`). Each node
 * is either a folder or a leaf file; folders carry an array of indices into
 * the same node pool for their children.
 *
 * The "visible projection" is rebuilt from the persistent expansion state
 * after every mutation. This costs O(visible) per mutation but keeps the
 * render path trivially indexable, which matches our virtualized-list
 * rendering plans for large directories.
 *
 * Sort order per directory: directories first, then files; ties broken by
 * case-insensitive name compare. Hidden entries (leading '.') are filtered
 * out — Sol won't show dotfiles in this pass.
 */

#include "sol_file_tree.h"

#include "sol_platform.h"

#include <causality.h>   /* Ca_Signal, ca_signal_set_u32, ca_signal_get_u32 */

#include "sol_event.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Internal types                                                    */
/* ---------------------------------------------------------------- */

typedef struct Node {
    char    *name;        /* owned */
    char    *full_path;   /* owned */
    bool     is_dir;
    bool     expanded;    /* meaningful for dirs */
    bool     loaded;      /* children already scanned                  */
    size_t  *children;    /* owned: indices into tree->nodes           */
    size_t   child_count;
} Node;

struct SolFileTree {
    char   *root_path;       /* owned, NULL if unset                       */
    Node   *nodes;           /* owned pool                                 */
    size_t  node_count;
    size_t  node_capacity;
    size_t  root_index;      /* index of the synthetic root in nodes[]     */

    SolFileEntry *visible;   /* owned, rebuilt on every mutation           */
    size_t        visible_count;
    size_t        visible_capacity;

    /* Optional caller-owned u32 revision signal. Bumped after every
       successful mutation so causality effects that read it re-run. */
    Ca_Signal *rev;

    /* Optional caller-owned event bus. When attached, the tree
       publishes sol.file_tree.root_changed on every set_root success. */
    SolEventBus *events;
};

/* Increment the attached revision signal to notify reactive subscribers. */
static void bump_rev(SolFileTree *t)
{
    if (t && t->rev) {
        ca_signal_set_u32(t->rev, ca_signal_get_u32(t->rev) + 1u);
    }
}

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

/* Duplicate a string into a heap-allocated buffer; NULL input returns NULL. */
static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1u);
    if (!o) return NULL;
    memcpy(o, s, n + 1u);
    return o;
}

/* Join a parent directory path and a child name into a new heap string. */
static char *path_join(const char *parent, const char *name)
{
    return sol_platform_path_join(parent, name);
}

/* Case-insensitive string comparison; returns negative/zero/positive like strcmp. */
static int casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

/* ---------------------------------------------------------------- */
/* Node pool                                                         */
/* ---------------------------------------------------------------- */

/*
 * Grow the node pool to hold at least needed entries.
 *
 * t       File tree whose node pool is grown.
 * needed  Minimum required capacity.
 * Returns true on success or if capacity is already sufficient.
 */
static bool ensure_node_capacity(SolFileTree *t, size_t needed)
{
    if (needed <= t->node_capacity) return true;
    size_t cap = t->node_capacity ? t->node_capacity : 16u;
    while (cap < needed) cap *= 2u;
    Node *nn = (Node *)realloc(t->nodes, cap * sizeof(Node));
    if (!nn) return false;
    t->nodes = nn;
    t->node_capacity = cap;
    return true;
}

/*
 * Allocate a new node in the pool and return its index.
 *
 * t        File tree to allocate within.
 * name     Display name of the entry (will be duplicated).
 * full     Full path of the entry (will be duplicated).
 * is_dir   Whether the entry represents a directory.
 * Returns  Index of the new node, or (size_t)-1 on OOM.
 */
static size_t alloc_node(SolFileTree *t, const char *name, const char *full,
                         bool is_dir)
{
    if (!ensure_node_capacity(t, t->node_count + 1u)) return (size_t)-1;
    Node *n = &t->nodes[t->node_count];
    memset(n, 0, sizeof(*n));
    n->name      = xstrdup(name);
    n->full_path = xstrdup(full);
    n->is_dir    = is_dir;
    if (!n->name || !n->full_path) {
        free(n->name);
        free(n->full_path);
        return (size_t)-1;
    }
    return t->node_count++;
}

/* Free every node in the pool and reset pool metadata to empty. */
static void free_all_nodes(SolFileTree *t)
{
    for (size_t i = 0; i < t->node_count; ++i) {
        free(t->nodes[i].name);
        free(t->nodes[i].full_path);
        free(t->nodes[i].children);
    }
    free(t->nodes);
    t->nodes = NULL;
    t->node_count = 0u;
    t->node_capacity = 0u;
}

/* ---------------------------------------------------------------- */
/* Directory scan                                                    */
/* ---------------------------------------------------------------- */

typedef struct ScanItem {
    char *name;
    bool  is_dir;
} ScanItem;

/* qsort comparator: directories sort before files; ties use case-insensitive name order. */
static int scan_compare(const void *a, const void *b)
{
    const ScanItem *x = (const ScanItem *)a;
    const ScanItem *y = (const ScanItem *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return casecmp(x->name, y->name);
}

/* Populate node->children for the given directory node. Children are
 * allocated as new entries in the node pool. Returns false on OOM or if
 * the directory cannot be opened. */
static bool load_children(SolFileTree *t, size_t dir_idx)
{
    Node *dir = &t->nodes[dir_idx];
    if (dir->loaded) return true;
    dir->loaded = true;     /* set up-front so partial failures aren't retried in a loop */

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, dir->full_path)) {
        return false;
    }

    /* Collect raw names first, then sort, then realize as nodes. */
    ScanItem *items = NULL;
    size_t    item_count = 0u;
    size_t    item_cap = 0u;

    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        const char *name = entry.name;
        if (name[0] == '.') continue;     /* skip hidden + . / .. */

        bool is_dir = entry.is_directory;

        if (item_count == item_cap) {
            size_t nc = item_cap ? item_cap * 2u : 16u;
            ScanItem *ni = (ScanItem *)realloc(items, nc * sizeof(ScanItem));
            if (!ni) {
                break;
            }
            items = ni;
            item_cap = nc;
        }
        items[item_count].name   = xstrdup(name);
        items[item_count].is_dir = is_dir;
        if (!items[item_count].name) break;
        item_count++;
    }
    sol_platform_dir_close(&iter);

    qsort(items, item_count, sizeof(ScanItem), scan_compare);

    /* Allocate the indices array. */
    size_t *kids = NULL;
    if (item_count > 0u) {
        kids = (size_t *)malloc(item_count * sizeof(size_t));
        if (!kids) {
            for (size_t i = 0; i < item_count; ++i) free(items[i].name);
            free(items);
            return false;
        }
    }

    size_t built = 0u;
    for (size_t i = 0; i < item_count; ++i) {
        char *full = path_join(dir->full_path, items[i].name);
        if (!full) { free(items[i].name); continue; }
        size_t idx = alloc_node(t, items[i].name, full, items[i].is_dir);
        free(full);
        free(items[i].name);
        if (idx == (size_t)-1) continue;
        /* alloc_node may have moved t->nodes; re-resolve dir. */
        dir = &t->nodes[dir_idx];
        kids[built++] = idx;
    }
    free(items);

    dir->children    = kids;
    dir->child_count = built;
    return true;
}

/* ---------------------------------------------------------------- */
/* Visible projection                                                */
/* ---------------------------------------------------------------- */

/*
 * Grow the visible projection array to hold at least needed entries.
 *
 * t       File tree whose visible array is grown.
 * needed  Minimum required capacity.
 * Returns true on success or if capacity is already sufficient.
 */
static bool ensure_visible_capacity(SolFileTree *t, size_t needed)
{
    if (needed <= t->visible_capacity) return true;
    size_t cap = t->visible_capacity ? t->visible_capacity : 32u;
    while (cap < needed) cap *= 2u;
    SolFileEntry *nv = (SolFileEntry *)realloc(t->visible, cap * sizeof(SolFileEntry));
    if (!nv) return false;
    t->visible = nv;
    t->visible_capacity = cap;
    return true;
}

/*
 * Append a node to the visible projection at the given indentation depth.
 *
 * t         File tree to append to.
 * node_idx  Index of the node to emit.
 * depth     Visual nesting depth (0 = top-level under root).
 */
static void emit_visible(SolFileTree *t, size_t node_idx, size_t depth)
{
    if (!ensure_visible_capacity(t, t->visible_count + 1u)) return;
    Node *n = &t->nodes[node_idx];
    SolFileEntry *e = &t->visible[t->visible_count++];
    e->name      = n->name;
    e->full_path = n->full_path;
    e->depth     = depth;
    e->is_dir    = n->is_dir;
    e->expanded  = n->expanded;
}

/*
 * Recursively populate the visible projection under an expanded directory node.
 *
 * Lazy-loads children on first visit. Re-resolves the Node pointer after each
 * lazy load because the node pool realloc may move memory.
 *
 * t         File tree being rebuilt.
 * node_idx  Directory node to descend into.
 * depth     Visual nesting depth of node_idx's children.
 */
static void rebuild_visible_recursive(SolFileTree *t, size_t node_idx, size_t depth)
{
    Node *n = &t->nodes[node_idx];
    if (!n->is_dir || !n->expanded) return;
    if (!n->loaded) {
        /* Best-effort lazy load. Failures leave the dir empty but expanded. */
        load_children(t, node_idx);
        n = &t->nodes[node_idx];   /* pool may have moved */
    }
    /* Snapshot child list before recursing. Each recursive call may trigger
       lazy loading which reallocs t->nodes, invalidating raw Node* pointers
       held in parent frames. The children[] array is separately allocated and
       not affected by a nodes realloc, so snapshotting count + pointer here
       keeps this frame's iteration stable regardless of downstream reallocs. */
    size_t  child_count = n->child_count;
    size_t *children    = n->children;
    for (size_t i = 0; i < child_count; ++i) {
        size_t ci = children[i];
        emit_visible(t, ci, depth);
        if (t->nodes[ci].is_dir && t->nodes[ci].expanded) {
            rebuild_visible_recursive(t, ci, depth + 1u);
        }
    }
}

/* Reset and fully rebuild the visible projection from the current expansion state. */
static void rebuild_visible(SolFileTree *t)
{
    t->visible_count = 0u;
    if (t->node_count == 0u) return;
    /* Root itself is hidden — only its children are shown. */
    rebuild_visible_recursive(t, t->root_index, 0u);
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

/* Allocate and return a new, empty file tree. Returns NULL on OOM. */
SolFileTree *sol_file_tree_create(void)
{
    SolFileTree *t = (SolFileTree *)calloc(1u, sizeof(SolFileTree));
    return t;
}

/* Free all resources owned by the file tree. Passing NULL is a no-op. */
void sol_file_tree_destroy(SolFileTree *tree)
{
    if (!tree) return;
    free_all_nodes(tree);
    free(tree->visible);
    free(tree->root_path);
    free(tree);
}

/* Attach a Causality u32 signal that is bumped after every tree mutation. */
void sol_file_tree_attach_revision_signal(SolFileTree *tree, Ca_Signal *sig)
{
    if (!tree) return;
    tree->rev = sig;
}

/* Attach an event bus used to publish SOL_EVENT_FILE_TREE_ROOT on root changes. */
void sol_file_tree_attach_event_bus(SolFileTree *tree, SolEventBus *bus)
{
    if (!tree) return;
    tree->events = bus;
}

/*
 * Reset the tree to use root_path as the new root directory.
 *
 * Clears all existing nodes and the visible projection. Passing NULL clears
 * the tree without error. Publishes SOL_EVENT_FILE_TREE_ROOT if an event bus
 * is attached.
 *
 * tree       File tree to update.
 * root_path  Absolute path of the new root, or NULL to clear.
 * Returns    true on success (always true for NULL root_path).
 */
bool sol_file_tree_set_root(SolFileTree *tree, const char *root_path)
{
    if (!tree) return false;

    free_all_nodes(tree);
    tree->root_index = 0u;
    tree->visible_count = 0u;
    free(tree->root_path);
    tree->root_path = NULL;

    /* NULL root means "hide/clear explorer". This is a successful
       state transition and must notify reactive subscribers so the
       tree panel disappears immediately. */
    if (!root_path) {
        bump_rev(tree);
        if (tree->events) {
            SolFileTreeRootPayload payload;
            payload.path = NULL;
            sol_event_publish(tree->events, SOL_EVENT_FILE_TREE_ROOT,
                              &payload, sizeof(payload), tree);
        }
        return true;
    }

    SolPathInfo info;
    if (!sol_platform_get_path_info(root_path, &info) || !info.is_directory) {
        bump_rev(tree);
        if (tree->events) {
            SolFileTreeRootPayload payload;
            payload.path = NULL;
            sol_event_publish(tree->events, SOL_EVENT_FILE_TREE_ROOT,
                              &payload, sizeof(payload), tree);
        }
        return false;
    }

    tree->root_path = xstrdup(root_path);
    if (!tree->root_path) {
        bump_rev(tree);
        if (tree->events) {
            SolFileTreeRootPayload payload;
            payload.path = NULL;
            sol_event_publish(tree->events, SOL_EVENT_FILE_TREE_ROOT,
                              &payload, sizeof(payload), tree);
        }
        return false;
    }

    size_t root_idx = alloc_node(tree, root_path, root_path, true);
    if (root_idx == (size_t)-1) {
        free(tree->root_path);
        tree->root_path = NULL;
        bump_rev(tree);
        if (tree->events) {
            SolFileTreeRootPayload payload;
            payload.path = NULL;
            sol_event_publish(tree->events, SOL_EVENT_FILE_TREE_ROOT,
                              &payload, sizeof(payload), tree);
        }
        return false;
    }
    tree->root_index = root_idx;
    tree->nodes[root_idx].expanded = true;     /* root always open */
    rebuild_visible(tree);
    bump_rev(tree);
    if (tree->events) {
        SolFileTreeRootPayload payload;
        payload.path = tree->root_path;
        sol_event_publish(tree->events, SOL_EVENT_FILE_TREE_ROOT,
                           &payload, sizeof(payload), tree);
    }
    return true;
}

/* Return the current root path, or NULL if no root is set. */
const char *sol_file_tree_root(const SolFileTree *tree)
{
    return tree ? tree->root_path : NULL;
}

/* Return the number of entries in the current visible projection. */
size_t sol_file_tree_visible_count(const SolFileTree *tree)
{
    return tree ? tree->visible_count : 0u;
}

/*
 * Return the visible entry at the given index, or NULL if out of range.
 *
 * tree   File tree to query.
 * index  Zero-based row index into the visible projection.
 * Returns Pointer to the entry (valid until the next mutation), or NULL.
 */
const SolFileEntry *sol_file_tree_visible(const SolFileTree *tree, size_t index)
{
    if (!tree || index >= tree->visible_count) return NULL;
    return &tree->visible[index];
}

/*
 * Toggle the expanded/collapsed state of the directory at visible row index.
 *
 * Rebuilds the visible projection and bumps the revision signal on success.
 *
 * tree   File tree to mutate.
 * index  Row index of the directory to toggle.
 * Returns true if the toggle was applied (false for non-directory or OOB).
 */
bool sol_file_tree_toggle(SolFileTree *tree, size_t index)
{
    if (!tree || index >= tree->visible_count) return false;
    /* Map visible row → node index by full_path equality (cheap and avoids
     * threading a parallel index vector). */
    const char *path = tree->visible[index].full_path;
    size_t found = (size_t)-1;
    for (size_t i = 0; i < tree->node_count; ++i) {
        if (tree->nodes[i].full_path == path) { found = i; break; }
    }
    if (found == (size_t)-1) return false;
    if (!tree->nodes[found].is_dir) return false;
    tree->nodes[found].expanded = !tree->nodes[found].expanded;
    rebuild_visible(tree);
    bump_rev(tree);
    return true;
}

/*
 * Recursively re-expand and load directories whose path is in
 * saved_paths, mirroring what sol_file_tree_toggle would have produced
 * one toggle at a time. Unlike rebuild_visible_recursive (which only
 * lazy-loads nodes already marked expanded), this walks down through
 * *unloaded* children to find matches, since after a full node-pool
 * rebuild none of the previously-expanded child directories exist as
 * nodes yet — their paths are only known from the saved snapshot.
 *
 * t             File tree being restored.
 * node_idx      Directory node to consider expanding.
 * saved_paths   Sorted-free list of full paths that were expanded before.
 * saved_count   Number of entries in saved_paths.
 */
static void restore_expansion(SolFileTree *t, size_t node_idx,
                              char **saved_paths, size_t saved_count)
{
    Node *n = &t->nodes[node_idx];
    if (!n->is_dir) return;

    bool was_expanded = false;
    for (size_t j = 0; j < saved_count; ++j) {
        if (strcmp(n->full_path, saved_paths[j]) == 0) { was_expanded = true; break; }
    }
    if (!was_expanded) return;

    n->expanded = true;
    if (!load_children(t, node_idx)) return;
    n = &t->nodes[node_idx];   /* pool may have moved */

    size_t child_count = n->child_count;
    size_t *children    = n->children;
    for (size_t i = 0; i < child_count; ++i) {
        /* children[] is a separately-owned array unaffected by further
           node-pool reallocs from recursive load_children calls below,
           so this snapshot stays valid across the loop. */
        restore_expansion(t, children[i], saved_paths, saved_count);
    }
}

/*
 * Re-scan the root directory to pick up external filesystem changes,
 * preserving which directories were expanded and without publishing
 * SOL_EVENT_FILE_TREE_ROOT.
 *
 * Deliberately distinct from sol_file_tree_set_root: that function's
 * event means "the root path itself changed" (opening a different
 * folder) and is meant to be rare — subscribers (e.g. the git plugin)
 * treat it as a reason to discard and rebuild their own state from
 * scratch. This function means "contents under the same root may have
 * changed" and is meant to be called frequently (e.g. once per drained
 * filesystem-watcher batch) without triggering that unrelated churn.
 *
 * Collapses to no cached state and no visible rows if the root can no
 * longer be read (e.g. the directory was deleted out from under Sol).
 *
 * tree    File tree to refresh.
 * Returns true on success; false if no root is set or the root path
 *         cannot be re-read.
 */
bool sol_file_tree_refresh(SolFileTree *tree)
{
    if (!tree || !tree->root_path) {
        return false;
    }

    /* Snapshot which directories were expanded, by path, since node
       indices don't survive the rebuild below. */
    size_t expanded_count = 0u;
    for (size_t i = 0; i < tree->node_count; ++i) {
        if (tree->nodes[i].is_dir && tree->nodes[i].expanded) expanded_count++;
    }
    char **expanded_paths = NULL;
    if (expanded_count > 0u) {
        expanded_paths = (char **)malloc(expanded_count * sizeof(char *));
        if (!expanded_paths) return false;
        size_t k = 0u;
        for (size_t i = 0; i < tree->node_count; ++i) {
            if (tree->nodes[i].is_dir && tree->nodes[i].expanded) {
                expanded_paths[k] = xstrdup(tree->nodes[i].full_path);
                if (!expanded_paths[k]) {
                    for (size_t j = 0; j < k; ++j) free(expanded_paths[j]);
                    free(expanded_paths);
                    return false;
                }
                k++;
            }
        }
    }

    char *root = xstrdup(tree->root_path);
    if (!root) {
        for (size_t j = 0; j < expanded_count; ++j) free(expanded_paths[j]);
        free(expanded_paths);
        return false;
    }

    free_all_nodes(tree);
    tree->root_index = 0u;
    tree->visible_count = 0u;
    free(tree->root_path);
    tree->root_path = NULL;

    bool ok = false;
    SolPathInfo info;
    if (sol_platform_get_path_info(root, &info) && info.is_directory) {
        size_t root_idx = alloc_node(tree, root, root, true);
        if (root_idx != (size_t)-1) {
            tree->root_index = root_idx;
            tree->root_path = root;
            root = NULL;   /* ownership moved into the node/tree */
            tree->nodes[root_idx].expanded = true;

            /* Re-load and re-expand every directory that was expanded
               before, by path. A directory that was deleted or renamed
               just silently stays collapsed — nothing meaningful to
               restore for it. */
            if (!load_children(tree, tree->root_index)) {
                /* Root itself failed to enumerate (e.g. permission
                   revoked mid-session) — leave it as an empty, collapsed
                   root rather than treating the whole refresh as fatal;
                   the explorer just shows nothing until it recovers. */
            } else {
                Node *root_node = &tree->nodes[tree->root_index];
                size_t child_count = root_node->child_count;
                size_t *children   = root_node->children;
                for (size_t i = 0; i < child_count; ++i) {
                    restore_expansion(tree, children[i], expanded_paths, expanded_count);
                }
            }
            rebuild_visible(tree);
            ok = true;
        }
    }

    free(root);
    for (size_t j = 0; j < expanded_count; ++j) free(expanded_paths[j]);
    free(expanded_paths);
    bump_rev(tree);
    return ok;
}
