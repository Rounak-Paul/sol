// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_rope.c — B-tree rope implementation.
 *
 * Tree shape:
 *   - Internal nodes have between BRANCH_MIN and BRANCH_MAX children.
 *   - Leaves reference a slice [start, start+len) of a refcounted chunk.
 *   - The root may temporarily violate the min-children invariant; we
 *     normalise after every edit.
 *
 * Aggregated metrics (bytes / chars / lines) are kept on every node so
 * lookups and conversions are O(log N).
 *
 * Mutation strategy: insert / remove walk down to the leaf, perform the
 * point edit, and propagate splits/merges up. Splits raise a sibling
 * pointer; merges may collapse the root.
 */

#include "sol_rope.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Tunables ----------------------------------------------------- */

#define SOL_ROPE_LEAF_TARGET 4096u   /* preferred leaf payload size       */
#define SOL_ROPE_LEAF_MAX    8192u   /* split leaves larger than this     */
#define SOL_ROPE_BRANCH_MAX  8u
#define SOL_ROPE_BRANCH_MIN  4u

/* ---- Types -------------------------------------------------------- */

typedef struct SolRopeChunk {
    int      ref_count;
    bool     is_mmap;
    size_t   mmap_size;   /* size of the mapping (for munmap)         */
    uint8_t *base;        /* heap base for owned chunks; mapping base
                             for mmap'd chunks                        */
} SolRopeChunk;

typedef struct SolRopeNode {
    bool   is_leaf;
    /* Aggregated metrics across the subtree rooted at this node. */
    size_t bytes;
    size_t chars;
    size_t lines;
    union {
        struct {
            SolRopeChunk *chunk;
            uint32_t      start; /* byte offset within chunk->base    */
            uint32_t      len;   /* slice length in bytes             */
        } leaf;
        struct {
            struct SolRopeNode *children[SOL_ROPE_BRANCH_MAX];
            uint32_t            child_count;
        } branch;
    } as;
} SolRopeNode;

struct SolRope {
    SolRopeNode *root;   /* may be NULL when empty                    */
};

/* ---- Chunk refcount ----------------------------------------------- */

static SolRopeChunk *chunk_new_owned(const uint8_t *data, size_t len)
{
    SolRopeChunk *c = (SolRopeChunk *)malloc(sizeof(*c));
    if (!c) return NULL;
    c->ref_count = 0;
    c->is_mmap   = false;
    c->mmap_size = 0;
    c->base = (uint8_t *)malloc(len ? len : 1);
    if (!c->base) { free(c); return NULL; }
    if (data && len) memcpy(c->base, data, len);
    return c;
}

static SolRopeChunk *chunk_new_mmap(uint8_t *base, size_t mmap_size)
{
    SolRopeChunk *c = (SolRopeChunk *)malloc(sizeof(*c));
    if (!c) return NULL;
    c->ref_count = 0;
    c->is_mmap   = true;
    c->mmap_size = mmap_size;
    c->base      = base;
    return c;
}

static void chunk_retain(SolRopeChunk *c)
{
    if (c) c->ref_count++;
}

static void chunk_release(SolRopeChunk *c)
{
    if (!c) return;
    assert(c->ref_count > 0);
    if (--c->ref_count > 0) return;
    if (c->is_mmap) {
        if (c->base && c->mmap_size) munmap(c->base, c->mmap_size);
    } else {
        free(c->base);
    }
    free(c);
}

/* ---- UTF-8 / line metrics ----------------------------------------- */

static void slice_metrics(const uint8_t *p, size_t len,
                          size_t *out_chars, size_t *out_lines)
{
    size_t chars = 0, lines = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = p[i];
        if ((b & 0xC0u) != 0x80u) chars++;
        if (b == (uint8_t)'\n') lines++;
    }
    *out_chars = chars;
    *out_lines = lines;
}

/* ---- Node lifecycle ----------------------------------------------- */

static void node_recompute_branch_metrics(SolRopeNode *n)
{
    assert(!n->is_leaf);
    size_t b = 0, c = 0, l = 0;
    for (uint32_t i = 0; i < n->as.branch.child_count; ++i) {
        SolRopeNode *ch = n->as.branch.children[i];
        b += ch->bytes;
        c += ch->chars;
        l += ch->lines;
    }
    n->bytes = b;
    n->chars = c;
    n->lines = l;
}

static SolRopeNode *node_new_leaf(SolRopeChunk *chunk, uint32_t start, uint32_t len)
{
    SolRopeNode *n = (SolRopeNode *)malloc(sizeof(*n));
    if (!n) return NULL;
    n->is_leaf = true;
    n->as.leaf.chunk = chunk;
    n->as.leaf.start = start;
    n->as.leaf.len   = len;
    chunk_retain(chunk);
    size_t chars, lines;
    slice_metrics(chunk->base + start, len, &chars, &lines);
    n->bytes = len;
    n->chars = chars;
    n->lines = lines;
    return n;
}

static SolRopeNode *node_new_branch(void)
{
    SolRopeNode *n = (SolRopeNode *)malloc(sizeof(*n));
    if (!n) return NULL;
    n->is_leaf = false;
    n->as.branch.child_count = 0;
    n->bytes = n->chars = n->lines = 0;
    return n;
}

static void node_destroy(SolRopeNode *n)
{
    if (!n) return;
    if (n->is_leaf) {
        chunk_release(n->as.leaf.chunk);
    } else {
        for (uint32_t i = 0; i < n->as.branch.child_count; ++i)
            node_destroy(n->as.branch.children[i]);
    }
    free(n);
}

/* ---- Bottom-up tree builder --------------------------------------- */

/* Build a balanced tree from a flat array of leaves. Consumes the
   array contents (transferring ownership to the returned tree).
   Returns NULL on OOM (in which case the caller still owns the
   leaves and must destroy them). */
static SolRopeNode *build_tree_from_leaves(SolRopeNode **leaves, size_t count)
{
    if (count == 0) return NULL;
    if (count == 1) return leaves[0];

    /* Each pass groups the current level into parents. Distribute
       children as evenly as possible so every parent has at least
       floor(level_count / parent_count) >= BRANCH_MIN children
       whenever level_count >= BRANCH_MIN. The root may legitimately
       end up with fewer; normalise_root() collapses single-child
       chains afterwards. */
    SolRopeNode **level         = leaves;
    size_t        level_count   = count;
    SolRopeNode **owned_buffer  = NULL;

    while (level_count > 1) {
        size_t parent_count = (level_count + SOL_ROPE_BRANCH_MAX - 1) / SOL_ROPE_BRANCH_MAX;
        size_t base_take    = level_count / parent_count;
        size_t extra        = level_count - base_take * parent_count;

        SolRopeNode **parents = (SolRopeNode **)malloc(parent_count * sizeof(*parents));
        if (!parents) {
            free(owned_buffer);
            return NULL;
        }

        size_t cursor = 0;
        for (size_t p = 0; p < parent_count; ++p) {
            size_t take = base_take + (p < extra ? 1u : 0u);
            SolRopeNode *parent = node_new_branch();
            if (!parent) {
                /* Roll back: detach already-built parents' children
                   (they remain owned by `level[]`) then free parents. */
                for (size_t k = 0; k < p; ++k) {
                    parents[k]->as.branch.child_count = 0;
                    free(parents[k]);
                }
                free(parents);
                free(owned_buffer);
                return NULL;
            }
            for (size_t k = 0; k < take; ++k)
                parent->as.branch.children[k] = level[cursor + k];
            parent->as.branch.child_count = (uint32_t)take;
            node_recompute_branch_metrics(parent);
            parents[p] = parent;
            cursor    += take;
        }

        free(owned_buffer);
        owned_buffer = parents;
        level        = parents;
        level_count  = parent_count;
    }

    SolRopeNode *root = level[0];
    free(owned_buffer);
    return root;
}

/* ---- Construction helpers ----------------------------------------- */

/* Slice an existing chunk into a sequence of leaves of size <= LEAF_TARGET.
   On OOM, frees any partial leaves and returns NULL. */
static SolRopeNode **leaves_from_chunk(SolRopeChunk *chunk, size_t total_len,
                                       size_t *out_count)
{
    size_t leaf_count = (total_len + SOL_ROPE_LEAF_TARGET - 1) / SOL_ROPE_LEAF_TARGET;
    if (leaf_count == 0) leaf_count = 1;
    SolRopeNode **leaves = (SolRopeNode **)malloc(leaf_count * sizeof(*leaves));
    if (!leaves) return NULL;

    size_t pos = 0, i = 0;
    while (pos < total_len) {
        size_t take = total_len - pos;
        if (take > SOL_ROPE_LEAF_TARGET) take = SOL_ROPE_LEAF_TARGET;
        leaves[i] = node_new_leaf(chunk, (uint32_t)pos, (uint32_t)take);
        if (!leaves[i]) {
            for (size_t k = 0; k < i; ++k) node_destroy(leaves[k]);
            free(leaves);
            return NULL;
        }
        pos += take;
        i++;
    }
    if (total_len == 0) {
        leaves[0] = node_new_leaf(chunk, 0, 0);
        if (!leaves[0]) { free(leaves); return NULL; }
        i = 1;
    }
    *out_count = i;
    return leaves;
}

SolRope *sol_rope_create(void)
{
    SolRope *r = (SolRope *)malloc(sizeof(*r));
    if (!r) return NULL;
    r->root = NULL;
    return r;
}

SolRope *sol_rope_from_bytes(const uint8_t *data, size_t len)
{
    SolRope *r = sol_rope_create();
    if (!r) return NULL;
    if (len == 0) return r;

    SolRopeChunk *chunk = chunk_new_owned(data, len);
    if (!chunk) { sol_rope_destroy(r); return NULL; }

    size_t leaf_count = 0;
    SolRopeNode **leaves = leaves_from_chunk(chunk, len, &leaf_count);
    if (!leaves) {
        /* No leaves yet hold the chunk; free it directly. */
        free(chunk->base);
        free(chunk);
        sol_rope_destroy(r);
        return NULL;
    }
    SolRopeNode *root = build_tree_from_leaves(leaves, leaf_count);
    if (!root) {
        for (size_t i = 0; i < leaf_count; ++i) node_destroy(leaves[i]);
        free(leaves);
        sol_rope_destroy(r);
        return NULL;
    }
    free(leaves);
    r->root = root;
    return r;
}

SolRope *sol_rope_from_file(const char *path, const char **out_error)
{
    if (!path) { if (out_error) *out_error = "null path"; return NULL; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (out_error) *out_error = "open() failed"; return NULL; }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        if (out_error) *out_error = "fstat() failed";
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;

    if (size == 0) {
        close(fd);
        return sol_rope_create();
    }

    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        if (out_error) *out_error = "mmap() failed";
        return NULL;
    }

    SolRopeChunk *chunk = chunk_new_mmap((uint8_t *)map, size);
    if (!chunk) {
        munmap(map, size);
        if (out_error) *out_error = "out of memory";
        return NULL;
    }

    SolRope *r = sol_rope_create();
    if (!r) {
        munmap(map, size);
        free(chunk);
        if (out_error) *out_error = "out of memory";
        return NULL;
    }

    size_t leaf_count = 0;
    SolRopeNode **leaves = leaves_from_chunk(chunk, size, &leaf_count);
    if (!leaves) {
        /* No leaves retained the chunk yet. */
        munmap(map, size);
        free(chunk);
        sol_rope_destroy(r);
        if (out_error) *out_error = "out of memory";
        return NULL;
    }
    SolRopeNode *root = build_tree_from_leaves(leaves, leaf_count);
    if (!root) {
        for (size_t i = 0; i < leaf_count; ++i) node_destroy(leaves[i]);
        free(leaves);
        sol_rope_destroy(r);
        if (out_error) *out_error = "out of memory";
        return NULL;
    }
    free(leaves);
    r->root = root;
    return r;
}

void sol_rope_destroy(SolRope *rope)
{
    if (!rope) return;
    node_destroy(rope->root);
    free(rope);
}

/* ---- Metrics ------------------------------------------------------ */

size_t sol_rope_byte_len(const SolRope *r)   { return r && r->root ? r->root->bytes : 0; }
size_t sol_rope_char_len(const SolRope *r)   { return r && r->root ? r->root->chars : 0; }
size_t sol_rope_line_count(const SolRope *r) { return r && r->root ? r->root->lines : 0; }

/* ---- Index conversions -------------------------------------------- */

/* Walk the tree to find the byte offset where the `target`-th newline
   ends. Line 0 starts at byte 0; line k (1..lines) starts at the byte
   immediately after the k-th newline. */
static size_t find_line_start(const SolRopeNode *n, size_t target,
                              size_t byte_acc)
{
    if (n->is_leaf) {
        if (target == 0) return byte_acc;
        const uint8_t *p = n->as.leaf.chunk->base + n->as.leaf.start;
        size_t seen = 0;
        for (uint32_t i = 0; i < n->as.leaf.len; ++i) {
            if (p[i] == (uint8_t)'\n') {
                seen++;
                if (seen == target) return byte_acc + i + 1;
            }
        }
        return byte_acc + n->as.leaf.len; /* shouldn't happen if caller clamps */
    }
    for (uint32_t i = 0; i < n->as.branch.child_count; ++i) {
        SolRopeNode *ch = n->as.branch.children[i];
        if (target <= ch->lines) return find_line_start(ch, target, byte_acc);
        target    -= ch->lines;
        byte_acc  += ch->bytes;
    }
    return byte_acc;
}

size_t sol_rope_byte_of_line(const SolRope *r, size_t line)
{
    if (!r || !r->root) return 0;
    if (line == 0) return 0;
    if (line > r->root->lines) return r->root->bytes;
    return find_line_start(r->root, line, 0);
}

static size_t count_newlines_to_offset(const SolRopeNode *n, size_t byte_off,
                                       size_t lines_acc)
{
    if (n->is_leaf) {
        const uint8_t *p = n->as.leaf.chunk->base + n->as.leaf.start;
        size_t lim = byte_off < n->as.leaf.len ? byte_off : n->as.leaf.len;
        for (size_t i = 0; i < lim; ++i)
            if (p[i] == (uint8_t)'\n') lines_acc++;
        return lines_acc;
    }
    for (uint32_t i = 0; i < n->as.branch.child_count; ++i) {
        SolRopeNode *ch = n->as.branch.children[i];
        if (byte_off <= ch->bytes)
            return count_newlines_to_offset(ch, byte_off, lines_acc);
        byte_off  -= ch->bytes;
        lines_acc += ch->lines;
    }
    return lines_acc;
}

size_t sol_rope_line_of_byte(const SolRope *r, size_t byte)
{
    if (!r || !r->root) return 0;
    if (byte >= r->root->bytes) return r->root->lines;
    return count_newlines_to_offset(r->root, byte, 0);
}

/* ---- Read --------------------------------------------------------- */

static size_t node_read(const SolRopeNode *n, size_t off, uint8_t *out, size_t max)
{
    if (n->is_leaf) {
        if (off >= n->as.leaf.len) return 0;
        size_t avail = n->as.leaf.len - off;
        size_t take  = avail < max ? avail : max;
        memcpy(out, n->as.leaf.chunk->base + n->as.leaf.start + off, take);
        return take;
    }
    size_t copied = 0;
    for (uint32_t i = 0; i < n->as.branch.child_count && copied < max; ++i) {
        SolRopeNode *ch = n->as.branch.children[i];
        if (off >= ch->bytes) { off -= ch->bytes; continue; }
        size_t got = node_read(ch, off, out + copied, max - copied);
        copied += got;
        off = 0;
    }
    return copied;
}

size_t sol_rope_read(const SolRope *r, size_t byte_offset,
                     uint8_t *out, size_t max)
{
    if (!r || !r->root || !out || max == 0) return 0;
    if (byte_offset >= r->root->bytes) return 0;
    return node_read(r->root, byte_offset, out, max);
}

/* ---- Chunk iterator ----------------------------------------------- */

/* The iterator stack holds parent frames as encoded {node_ptr | child_index}
   in the void* slot. We split the stack into parallel arrays packed into
   the single `stack` field by using two halves: nodes in the lower half,
   indices encoded as offsets in the upper half. To stay simple and ABI-
   stable, we cast each slot to a small struct via memcpy. */

typedef struct IterFrame {
    const SolRopeNode *node;
    uint32_t           idx;
} IterFrame;

_Static_assert(sizeof(IterFrame) <= sizeof(void *) * 2, "iter frame fits");

static void iter_push(SolRopeChunkIter *it, const SolRopeNode *n, uint32_t idx)
{
    /* Use two slots per frame so we can store the full IterFrame. */
    if (it->depth + 2 > sizeof(it->stack) / sizeof(it->stack[0])) return;
    IterFrame f = { n, idx };
    memcpy(&it->stack[it->depth], &f, sizeof f);
    it->depth += 2;
}

static bool iter_pop(SolRopeChunkIter *it, IterFrame *out)
{
    if (it->depth < 2) return false;
    it->depth -= 2;
    memcpy(out, &it->stack[it->depth], sizeof *out);
    return true;
}

void sol_rope_chunk_iter_init(SolRopeChunkIter *it, const SolRope *rope)
{
    it->rope     = rope;
    it->depth    = 0;
    it->byte_pos = 0;
    if (rope && rope->root) iter_push(it, rope->root, 0);
}

bool sol_rope_chunk_iter_next(SolRopeChunkIter *it,
                              const uint8_t **out_data, size_t *out_len,
                              size_t *out_byte_offset)
{
    while (it->depth >= 2) {
        IterFrame f;
        memcpy(&f, &it->stack[it->depth - 2], sizeof f);

        if (f.node->is_leaf) {
            it->depth -= 2;
            if (out_data) *out_data = f.node->as.leaf.chunk->base + f.node->as.leaf.start;
            if (out_len)  *out_len  = f.node->as.leaf.len;
            if (out_byte_offset) *out_byte_offset = it->byte_pos;
            it->byte_pos += f.node->as.leaf.len;
            return true;
        }

        if (f.idx >= f.node->as.branch.child_count) {
            (void)iter_pop(it, &f);
            continue;
        }

        /* Update parent's idx in place, then descend. */
        IterFrame nf = { f.node, f.idx + 1 };
        memcpy(&it->stack[it->depth - 2], &nf, sizeof nf);
        iter_push(it, f.node->as.branch.children[f.idx], 0);
    }
    return false;
}

/* ---- Edit: remove ------------------------------------------------- */

/* Forward decls. */
static SolRopeNode *node_clone_leaf_slice(const SolRopeNode *leaf,
                                          uint32_t new_start, uint32_t new_len);
static void normalise_root(SolRope *r);

/* Remove [off, off+len) within node n. May replace n with a different
   node (or NULL if the result is empty). Returns the new node. The
   old `n` is consumed (destroyed or repurposed). */
static SolRopeNode *node_remove(SolRopeNode *n, size_t off, size_t len)
{
    if (len == 0) return n;

    if (n->is_leaf) {
        if (off == 0 && len >= n->as.leaf.len) {
            node_destroy(n);
            return NULL;
        }
        /* Build replacement leaves for the surviving prefix/suffix. */
        SolRopeNode *prefix = NULL, *suffix = NULL;
        if (off > 0) {
            prefix = node_clone_leaf_slice(n, n->as.leaf.start, (uint32_t)off);
            if (!prefix) return n;   /* OOM: leave node unchanged. */
        }
        size_t end = off + len;
        if (end > n->as.leaf.len) end = n->as.leaf.len;
        if (end < n->as.leaf.len) {
            suffix = node_clone_leaf_slice(n, n->as.leaf.start + (uint32_t)end,
                                           n->as.leaf.len - (uint32_t)end);
            if (!suffix) { node_destroy(prefix); return n; }
        }
        node_destroy(n);
        if (prefix && suffix) {
            SolRopeNode *parent = node_new_branch();
            if (!parent) { node_destroy(prefix); node_destroy(suffix); return NULL; }
            parent->as.branch.children[0] = prefix;
            parent->as.branch.children[1] = suffix;
            parent->as.branch.child_count = 2;
            node_recompute_branch_metrics(parent);
            return parent;
        }
        return prefix ? prefix : suffix;
    }

    /* Branch: descend into each affected child. */
    uint32_t i = 0;
    while (i < n->as.branch.child_count && len > 0) {
        SolRopeNode *ch = n->as.branch.children[i];
        if (off >= ch->bytes) { off -= ch->bytes; i++; continue; }

        size_t avail   = ch->bytes - off;
        size_t take    = len < avail ? len : avail;
        SolRopeNode *new_ch = node_remove(ch, off, take);
        len -= take;
        off  = 0;

        if (new_ch == NULL) {
            for (uint32_t k = i; k + 1 < n->as.branch.child_count; ++k)
                n->as.branch.children[k] = n->as.branch.children[k + 1];
            n->as.branch.child_count--;
        } else {
            n->as.branch.children[i] = new_ch;
            i++;
        }
    }
    if (n->as.branch.child_count == 0) {
        free(n);
        return NULL;
    }
    if (n->as.branch.child_count == 1) {
        SolRopeNode *only = n->as.branch.children[0];
        free(n);
        return only;
    }
    node_recompute_branch_metrics(n);
    return n;
}

/* ---- Edit: insert ------------------------------------------------- */

/* Insert `new_node` into branch `parent` at child position `at`. If the
   branch overflows, returns a freshly allocated sibling holding the
   right half via *out_split; otherwise *out_split = NULL. Returns
   false on OOM (parent is left in a consistent pre-call state). */
static bool branch_insert_child_split(SolRopeNode *parent, uint32_t at,
                                      SolRopeNode *new_node,
                                      SolRopeNode **out_split)
{
    *out_split = NULL;
    assert(!parent->is_leaf && at <= parent->as.branch.child_count);

    if (parent->as.branch.child_count < SOL_ROPE_BRANCH_MAX) {
        for (uint32_t k = parent->as.branch.child_count; k > at; --k)
            parent->as.branch.children[k] = parent->as.branch.children[k - 1];
        parent->as.branch.children[at] = new_node;
        parent->as.branch.child_count++;
        node_recompute_branch_metrics(parent);
        return true;
    }

    /* Overflow: build a temporary array of MAX+1 children, then split. */
    SolRopeNode *tmp[SOL_ROPE_BRANCH_MAX + 1];
    for (uint32_t k = 0; k < at; ++k) tmp[k] = parent->as.branch.children[k];
    tmp[at] = new_node;
    for (uint32_t k = at; k < parent->as.branch.child_count; ++k)
        tmp[k + 1] = parent->as.branch.children[k];
    uint32_t total = parent->as.branch.child_count + 1;

    SolRopeNode *sibling = node_new_branch();
    if (!sibling) return false;

    uint32_t left_count  = total / 2;
    uint32_t right_count = total - left_count;
    for (uint32_t k = 0; k < left_count;  ++k) parent->as.branch.children[k]  = tmp[k];
    for (uint32_t k = 0; k < right_count; ++k) sibling->as.branch.children[k] = tmp[left_count + k];
    parent->as.branch.child_count  = left_count;
    sibling->as.branch.child_count = right_count;
    node_recompute_branch_metrics(parent);
    node_recompute_branch_metrics(sibling);
    *out_split = sibling;
    return true;
}

/* Recursive insert. On success, *out_node receives the (possibly new)
   replacement for the subtree rooted at n, and *out_split (if non-NULL)
   receives a sibling node when the parent must adopt one. */
static bool node_insert(SolRopeNode *n, size_t off,
                        SolRopeNode **leaves, size_t leaf_count,
                        SolRopeNode **out_node, SolRopeNode **out_split)
{
    *out_split = NULL;

    if (n->is_leaf) {
        /* Split the existing leaf into prefix + new_leaves + suffix,
           wrap in a branch, then let the caller flatten/rebalance. */
        SolRopeNode *prefix = NULL, *suffix = NULL;
        if (off > 0) {
            prefix = node_clone_leaf_slice(n, n->as.leaf.start, (uint32_t)off);
            if (!prefix) return false;
        }
        if (off < n->as.leaf.len) {
            suffix = node_clone_leaf_slice(n,
                                           n->as.leaf.start + (uint32_t)off,
                                           n->as.leaf.len - (uint32_t)off);
            if (!suffix) { node_destroy(prefix); return false; }
        }
        node_destroy(n);

        size_t group_count = leaf_count + (prefix ? 1u : 0u) + (suffix ? 1u : 0u);
        SolRopeNode **group = (SolRopeNode **)malloc(group_count * sizeof(*group));
        if (!group) {
            node_destroy(prefix); node_destroy(suffix);
            for (size_t i = 0; i < leaf_count; ++i) node_destroy(leaves[i]);
            return false;
        }
        size_t gi = 0;
        if (prefix) group[gi++] = prefix;
        for (size_t i = 0; i < leaf_count; ++i) group[gi++] = leaves[i];
        if (suffix) group[gi++] = suffix;

        SolRopeNode *sub = build_tree_from_leaves(group, group_count);
        free(group);
        if (!sub) {
            /* OOM mid-mutation: the original n was already destroyed and
               the new leaves cannot be reattached cleanly. Editors
               cannot meaningfully recover from heap exhaustion mid-
               edit; abort rather than corrupt the tree. */
            abort();
        }
        *out_node = sub;
        return true;
    }

    /* Branch: find child whose range covers `off`. */
    uint32_t i = 0;
    while (i + 1 < n->as.branch.child_count && off > n->as.branch.children[i]->bytes) {
        off -= n->as.branch.children[i]->bytes;
        i++;
    }
    if (off > n->as.branch.children[i]->bytes) off = n->as.branch.children[i]->bytes;

    SolRopeNode *new_child = NULL, *child_split = NULL;
    if (!node_insert(n->as.branch.children[i], off, leaves, leaf_count,
                     &new_child, &child_split))
        return false;
    n->as.branch.children[i] = new_child;

    if (child_split) {
        SolRopeNode *split = NULL;
        if (!branch_insert_child_split(n, i + 1, child_split, &split)) {
            /* OOM mid-mutation; see comment in the leaf branch above. */
            abort();
        }
        *out_split = split;
    } else {
        node_recompute_branch_metrics(n);
    }
    *out_node = n;
    return true;
}

/* ---- Helpers ------------------------------------------------------ */

static SolRopeNode *node_clone_leaf_slice(const SolRopeNode *leaf,
                                          uint32_t new_start, uint32_t new_len)
{
    assert(leaf->is_leaf);
    return node_new_leaf(leaf->as.leaf.chunk, new_start, new_len);
}

static void normalise_root(SolRope *r)
{
    /* Collapse single-child root chains. */
    while (r->root && !r->root->is_leaf && r->root->as.branch.child_count == 1) {
        SolRopeNode *only = r->root->as.branch.children[0];
        free(r->root);
        r->root = only;
    }
}

/* ---- Public edit entry points ------------------------------------- */

bool sol_rope_insert(SolRope *r, size_t at, const uint8_t *data, size_t len)
{
    if (!r) return false;
    size_t total = r->root ? r->root->bytes : 0;
    if (at > total) return false;
    if (len == 0) return true;
    if (!data) return false;

    /* Build a fresh chunk and slice it into target-sized leaves. */
    SolRopeChunk *chunk = chunk_new_owned(data, len);
    if (!chunk) return false;
    size_t leaf_count = 0;
    SolRopeNode **leaves = leaves_from_chunk(chunk, len, &leaf_count);
    if (!leaves) {
        free(chunk->base); free(chunk);
        return false;
    }

    if (!r->root) {
        SolRopeNode *root = build_tree_from_leaves(leaves, leaf_count);
        if (!root) {
            for (size_t i = 0; i < leaf_count; ++i) node_destroy(leaves[i]);
            free(leaves);
            return false;
        }
        free(leaves);
        r->root = root;
        return true;
    }

    SolRopeNode *new_root = NULL, *split = NULL;
    bool ok = node_insert(r->root, at, leaves, leaf_count, &new_root, &split);
    free(leaves);
    if (!ok) return false;
    r->root = new_root;
    if (split) {
        SolRopeNode *parent = node_new_branch();
        if (!parent) {
            /* Tree is consistent (root absorbed the edit); a new root
               level is what failed. Abort rather than silently drop
               half of the inserted data. */
            abort();
        }
        parent->as.branch.children[0] = r->root;
        parent->as.branch.children[1] = split;
        parent->as.branch.child_count = 2;
        node_recompute_branch_metrics(parent);
        r->root = parent;
    }
    normalise_root(r);
    return true;
}

bool sol_rope_remove(SolRope *r, size_t at, size_t len)
{
    if (!r || !r->root) return true;
    size_t total = r->root->bytes;
    if (at >= total || len == 0) return true;
    if (at + len > total) len = total - at;

    r->root = node_remove(r->root, at, len);
    normalise_root(r);
    return true;
}
