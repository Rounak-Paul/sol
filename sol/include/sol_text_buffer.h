// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.
//
// sol_text_buffer.h — text storage for the sol editor.
//
// Backed by a classic two-buffer piece table:
//   - "original" buffer: the loaded-from-disk bytes (immutable)
//   - "add" buffer: append-only, holds every keystroke ever typed
//   - a doubly-linked list of pieces selects substrings of either
//     buffer, in document order.
//
// Edits are O(1) amortised at the piece-list level; full-text reads
// stitch pieces on demand. This keeps memory predictable for large
// files without paying continuous-allocation costs on every keystroke.
//
// Encoding: UTF-8 in/out. The buffer stores bytes verbatim and never
// transcodes. Line tracking is byte-offset-based; users that need
// columns in code points can iterate one piece at a time.
//
// Threading: not thread-safe. Editing is main-thread-only. Loading
// from disk should happen on a worker thread, then the resulting
// SolTextBuffer is handed to the main thread.

#ifndef SOL_TEXT_BUFFER_H
#define SOL_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolTextBuffer SolTextBuffer;

/* Lifecycle ------------------------------------------------------------- */

SolTextBuffer *sol_text_buffer_create(void);

/* Take ownership of `bytes` (must be malloc'd) — buffer will free it
   on destroy. Use this after loading a file with sol_file_read_all. */
SolTextBuffer *sol_text_buffer_create_from_owned(char *bytes, size_t length);

void sol_text_buffer_destroy(SolTextBuffer *buf);

/* Queries --------------------------------------------------------------- */

size_t sol_text_buffer_length(const SolTextBuffer *buf);          /* bytes */
size_t sol_text_buffer_line_count(const SolTextBuffer *buf);

/* Copy [start, start+max_bytes) of the document into `out`. Returns the
   number of bytes actually copied (may be less than max_bytes near EOF).
   No NUL terminator is appended. */
size_t sol_text_buffer_read(const SolTextBuffer *buf,
                            size_t start,
                            char *out,
                            size_t max_bytes);

/* Allocate a NUL-terminated copy of the entire document. Caller frees. */
char *sol_text_buffer_to_cstring(const SolTextBuffer *buf);

/* Mutations ------------------------------------------------------------- */

bool sol_text_buffer_insert(SolTextBuffer *buf,
                            size_t offset,
                            const char *bytes,
                            size_t length);

bool sol_text_buffer_erase(SolTextBuffer *buf, size_t offset, size_t length);

/* Convenience: insert a single UTF-8 codepoint encoded as 1-4 bytes. */
bool sol_text_buffer_insert_char(SolTextBuffer *buf, size_t offset, uint32_t codepoint);

/* Dirty tracking -------------------------------------------------------- */

bool sol_text_buffer_dirty(const SolTextBuffer *buf);
void sol_text_buffer_mark_clean(SolTextBuffer *buf);

/* Generation counter — bumped on every successful mutation. Wire this
   into a Ca_Signal to drive reactive redraws. */
uint64_t sol_text_buffer_generation(const SolTextBuffer *buf);

#ifdef __cplusplus
}
#endif

#endif
