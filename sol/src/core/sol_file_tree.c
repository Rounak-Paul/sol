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

#include <causality.h>   /* Ca_Signal, ca_signal_set_u32, ca_signal_get_u32 */

#include "sol_event.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static void bump_rev(SolFileTree *t)
{
    if (t && t->rev) {
        ca_signal_set_u32(t->rev, ca_signal_get_u32(t->rev) + 1u);
    }
}

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1u);
    if (!o) return NULL;
    memcpy(o, s, n + 1u);
    return o;
}

static char *path_join(const char *parent, const char *name)
{
    size_t pn = strlen(parent);
    size_t nn = strlen(name);
    bool need_sep = (pn > 0u && parent[pn - 1u] != '/');
    char *out = (char *)malloc(pn + (need_sep ? 1u : 0u) + nn + 1u);
    if (!out) return NULL;
    memcpy(out, parent, pn);
    size_t off = pn;
    if (need_sep) out[off++] = '/';
    memcpy(out + off, name, nn + 1u);
    return out;
}

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

    DIR *d = opendir(dir->full_path);
    if (!d) return false;

    /* Collect raw names first, then sort, then realize as nodes. */
    ScanItem *items = NULL;
    size_t    item_count = 0u;
    size_t    item_cap = 0u;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;     /* skip hidden + . / .. */

        char *full = path_join(dir->full_path, name);
        if (!full) continue;

        struct stat st;
        bool is_dir = false;
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;

        if (item_count == item_cap) {
            size_t nc = item_cap ? item_cap * 2u : 16u;
            ScanItem *ni = (ScanItem *)realloc(items, nc * sizeof(ScanItem));
            if (!ni) { free(full); break; }
            items = ni;
            item_cap = nc;
        }
        items[item_count].name   = xstrdup(name);
        items[item_count].is_dir = is_dir;
        free(full);
        if (!items[item_count].name) break;
        item_count++;
    }
    closedir(d);

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

static void rebuild_visible_recursive(SolFileTree *t, size_t node_idx, size_t depth)
{
    Node *n = &t->nodes[node_idx];
    if (!n->is_dir || !n->expanded) return;
    if (!n->loaded) {
        /* Best-effort lazy load. Failures leave the dir empty but expanded. */
        load_children(t, node_idx);
        n = &t->nodes[node_idx];   /* pool may have moved */
    }
    for (size_t i = 0; i < n->child_count; ++i) {
        size_t ci = n->children[i];
        emit_visible(t, ci, depth);
        if (t->nodes[ci].is_dir && t->nodes[ci].expanded) {
            rebuild_visible_recursive(t, ci, depth + 1u);
        }
    }
}

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

SolFileTree *sol_file_tree_create(void)
{
    SolFileTree *t = (SolFileTree *)calloc(1u, sizeof(SolFileTree));
    return t;
}

void sol_file_tree_destroy(SolFileTree *tree)
{
    if (!tree) return;
    free_all_nodes(tree);
    free(tree->visible);
    free(tree->root_path);
    free(tree);
}

void sol_file_tree_attach_revision_signal(SolFileTree *tree, Ca_Signal *sig)
{
    if (!tree) return;
    tree->rev = sig;
}

void sol_file_tree_attach_event_bus(SolFileTree *tree, SolEventBus *bus)
{
    if (!tree) return;
    tree->events = bus;
}

bool sol_file_tree_set_root(SolFileTree *tree, const char *root_path)
{
    if (!tree) return false;

    free_all_nodes(tree);
    tree->visible_count = 0u;
    free(tree->root_path);
    tree->root_path = NULL;

    if (!root_path) return false;

    struct stat st;
    if (stat(root_path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    tree->root_path = xstrdup(root_path);
    if (!tree->root_path) return false;

    size_t root_idx = alloc_node(tree, root_path, root_path, true);
    if (root_idx == (size_t)-1) {
        free(tree->root_path);
        tree->root_path = NULL;
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

const char *sol_file_tree_root(const SolFileTree *tree)
{
    return tree ? tree->root_path : NULL;
}

size_t sol_file_tree_visible_count(const SolFileTree *tree)
{
    return tree ? tree->visible_count : 0u;
}

const SolFileEntry *sol_file_tree_visible(const SolFileTree *tree, size_t index)
{
    if (!tree || index >= tree->visible_count) return NULL;
    return &tree->visible[index];
}

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
