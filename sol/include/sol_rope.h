// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_rope.h — Sol's text rope.
 *
 * A B-tree rope intended as the storage backend for Sol's text buffers.
 * Designed for many large text files: leaves are slices of reference-
 * counted chunks, so an mmap'd file becomes a tree of zero-copy slices,
 * and edits replace only the affected leaves (copy-on-write).
 *
 * Indexing units (all maintained in O(log N)):
 *   - bytes      : raw byte length
 *   - chars      : UTF-8 code points (count of bytes whose top two bits
 *                  are not 10xxxxxx). Invalid sequences are still
 *                  counted by their leading bytes; the rope itself is
 *                  byte-clean and never rejects content.
 *   - lines      : count of '\n' bytes in the rope; the line count is
 *                  the number of LFs (so a non-empty buffer with no
 *                  trailing newline has lines == 0). Conversions treat
 *                  the buffer as having (lines + 1) line slots and the
 *                  start of line k (0-based) is the byte after the
 *                  k-th '\n'.
 *
 * The rope is not thread-safe. Pin it behind an outer lock or use one
 * rope per thread if you need concurrency.
 */

#ifndef SOL_ROPE_H
#define SOL_ROPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolRope SolRope;

/* ---- Lifecycle ----------------------------------------------------- */

/* Create an empty rope. Returns NULL on allocation failure. */
SolRope *sol_rope_create(void);

/*
 * Build a rope from an in-memory byte buffer.
 *
 * The bytes are copied into owned leaves; the caller may free data afterwards.
 *
 * data  Pointer to the source bytes.
 * len   Number of bytes to copy.
 * Returns  A new rope, or NULL on allocation failure.
 */
SolRope *sol_rope_from_bytes(const uint8_t *data, size_t len);

/*
 * Build a rope by memory-mapping a file (zero-copy until first edit).
 *
 * The mapping is shared by all leaves and unmapped when the last leaf is destroyed.
 *
 * path       Path of the file to map.
 * out_error  If non-NULL, receives a static error string on failure.
 * Returns    A new rope, or NULL on error.
 */
SolRope *sol_rope_from_file(const char *path, const char **out_error);

/* Release the rope and decrement the refcount of all leaf chunks. */
void sol_rope_destroy(SolRope *rope);

/* ---- Metrics ------------------------------------------------------- */

/* Returns the total byte length of the rope. */
size_t sol_rope_byte_len  (const SolRope *rope);

/* Returns the number of UTF-8 code points in the rope. */
size_t sol_rope_char_len  (const SolRope *rope);

/* Returns the number of newline characters in the rope. */
size_t sol_rope_line_count(const SolRope *rope);

/* ---- Index conversions -------------------------------------------- */

/*
 * Return the byte offset of the start of a line (0-based).
 *
 * rope  The rope.
 * line  Zero-based line index. line 0 returns 0; line == line_count returns
 *       the byte just past the final newline (or byte_len if no trailing newline).
 *       Out-of-range returns byte_len.
 */
size_t sol_rope_byte_of_line(const SolRope *rope, size_t line);

/*
 * Return the line index that contains a given byte offset.
 *
 * rope  The rope.
 * byte  Byte offset to locate. Values >= byte_len map to line_count.
 */
size_t sol_rope_line_of_byte(const SolRope *rope, size_t byte);

/* ---- Read --------------------------------------------------------- */

/*
 * Copy bytes from the rope into a caller-supplied buffer.
 *
 * rope         The rope.
 * byte_offset  Starting byte position in the rope.
 * out          Destination buffer.
 * max          Maximum number of bytes to copy.
 * Returns      Number of bytes actually copied (0 on out-of-range).
 */
size_t sol_rope_read(const SolRope *rope, size_t byte_offset,
                     uint8_t *out, size_t max);

/*
 * Zero-copy chunk iterator for streaming rope content in leaf order.
 *
 * The pointer returned in out_data is valid until the next mutation of
 * the rope. Use for rendering or hashing without a full copy.
 */
typedef struct SolRopeChunkIter {
    const SolRope *rope;
    void          *stack[32];   /* opaque; sized for trees up to ~4G nodes */
    size_t         depth;
    size_t         byte_pos;    /* byte offset of the current leaf start */
} SolRopeChunkIter;

/*
 * Initialise a chunk iterator at the beginning of a rope.
 *
 * it    Iterator to initialise.
 * rope  The rope to iterate.
 */
void sol_rope_chunk_iter_init(SolRopeChunkIter *it, const SolRope *rope);

/*
 * Advance to the next leaf chunk.
 *
 * it              The iterator.
 * out_data        Receives a pointer to the leaf's bytes (not NUL-terminated).
 * out_len         Receives the byte length of this chunk.
 * out_byte_offset Receives the byte offset of the chunk's first byte.
 * Returns         true while more chunks remain; false when exhausted.
 */
bool sol_rope_chunk_iter_next(SolRopeChunkIter *it,
                              const uint8_t **out_data, size_t *out_len,
                              size_t *out_byte_offset);

/* ---- Edit --------------------------------------------------------- */

/*
 * Insert bytes into the rope at a given byte offset.
 *
 * rope  The rope to mutate.
 * at    Insertion point; must be <= byte_len.
 * data  Bytes to insert (copied into an owned leaf).
 * len   Number of bytes to insert.
 * Returns  false on OOM or if at > byte_len.
 */
bool sol_rope_insert(SolRope *rope, size_t at, const uint8_t *data, size_t len);

/*
 * Remove bytes from the rope starting at a given byte offset.
 *
 * rope  The rope to mutate.
 * at    Start of the region to remove.
 * len   Number of bytes to remove; clamped to available bytes.
 * Returns  false on internal failure.
 */
bool sol_rope_remove(SolRope *rope, size_t at, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SOL_ROPE_H */
