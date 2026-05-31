// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_text_buffer.h — Text-kind buffers backed by a rope.
 *
 * One module owns ALL text-buffer state and editing primitives:
 *
 *   - Content storage         : SolRope (B-tree of shared chunks)
 *   - Cursor / scroll metrics : byte-offset caret, sticky preferred col,
 *                               top-of-viewport line index
 *   - Edit primitives         : codepoint insert, newline, backspace,
 *                               delete-forward, codepoint-wise motion
 *
 * The renderer (sol_text_view.h) and the input router are layered on
 * top; they only call the public API in this header.
 *
 * Concurrency: pin a buffer behind an outer lock if you need to read
 * it from a non-main thread. The rope itself is not thread-safe.
 */

#ifndef SOL_TEXT_BUFFER_H
#define SOL_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sol_buffer.h"
#include "sol_rope.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque editor state attached to every TEXT-kind SolBuffer. */
typedef struct SolTextBuffer SolTextBuffer;

/* ---- Open / find -------------------------------------------------- */

/* Open an empty text buffer. The render op is required for the buffer
   to appear on screen; passing NULL is legal but means the buffer is
   invisible (useful for tests). */
SolBufferId sol_text_buffer_open_empty(SolBufferSystem *system,
                                       const char *display_name,
                                       SolBufferRenderFn render);

/* Open a file-backed text buffer via mmap (zero-copy until first edit). */
SolBufferId sol_text_buffer_open_file(SolBufferSystem *system,
                                      const char *path,
                                      const char *display_name,
                                      SolBufferRenderFn render,
                                      const char **out_error);

/* Open a buffer from an in-memory string. `len` may be 0 with text
   == NULL for an empty buffer. `source_path` is optional and used for
   open-by-path dedup. */
SolBufferId sol_text_buffer_open_string(SolBufferSystem *system,
                                        const char *display_name,
                                        const char *text, size_t len,
                                        const char *source_path,
                                        SolBufferRenderFn render);

/* Find an open TEXT buffer whose source_path matches `path` (strcmp).
   Returns 0 when no match. Use for open-file dedup. */
SolBufferId sol_text_buffer_find_by_path(SolBufferSystem *system,
                                         const char *path);

/* ---- Accessors ---------------------------------------------------- */

/* Editor state attached to a TEXT buffer (NULL for non-text). */
SolTextBuffer *sol_text_buffer_state(SolBuffer *buffer);

/* Active text buffer's editor state (NULL if nothing text-kind focused). */
SolTextBuffer *sol_text_buffer_active(SolBufferSystem *system);

/* Borrow the underlying rope. NULL for non-text buffers. */
SolRope *sol_text_buffer_rope(SolBuffer *buffer);

const char *sol_text_buffer_source_path(const SolTextBuffer *tb);

/* Syntax highlighter attached to this buffer, or NULL if no language
 * was matched for the file extension.  Owned by the text buffer. */
typedef struct SolSyntaxHighlighter SolSyntaxHighlighter;
SolSyntaxHighlighter *sol_text_buffer_highlighter(const SolTextBuffer *tb);

/* ---- Cursor / scroll metrics ------------------------------------- */

size_t sol_text_buffer_cursor_byte (const SolTextBuffer *tb);
size_t sol_text_buffer_cursor_line (const SolTextBuffer *tb);
/* Byte offset within the cursor's line (0-based). */
size_t sol_text_buffer_cursor_col  (const SolTextBuffer *tb);

int  sol_text_buffer_scroll_top    (const SolTextBuffer *tb);
void sol_text_buffer_set_scroll_top(SolTextBuffer *tb, int line);

/* Force scroll_top so that the cursor sits inside `viewport_lines`. */
void sol_text_buffer_ensure_cursor_visible(SolTextBuffer *tb,
                                           int viewport_lines);

/* Total visible line count (always >= 1 — an empty buffer has one
   logical empty line). */
size_t sol_text_buffer_line_count(const SolTextBuffer *tb);

/* Copy line `line` (0-based) bytes into `out` (up to `max - 1`,
   leaving room for NUL). Returns the byte length actually copied
   (not counting the terminator). */
size_t sol_text_buffer_copy_line(const SolTextBuffer *tb, size_t line,
                                 char *out, size_t max);

/* Byte length of line `line`, excluding the trailing newline. */
size_t sol_text_buffer_line_len(const SolTextBuffer *tb, size_t line);

/* ---- Edits -------------------------------------------------------- */

bool sol_text_buffer_insert_codepoint(SolTextBuffer *tb, uint32_t cp);
bool sol_text_buffer_insert_newline  (SolTextBuffer *tb);
bool sol_text_buffer_backspace       (SolTextBuffer *tb);
bool sol_text_buffer_delete_forward  (SolTextBuffer *tb);

/* Raw byte-offset mutations.  These bypass cursor tracking but fire
 * SOL_EVENT_TEXT_EDITED so plugins and the UI stay in sync.
 * `sol_text_buffer_insert_bytes` inserts `len` bytes at `byte_offset`.
 * `sol_text_buffer_delete_bytes` removes `byte_count` bytes starting at
 * `byte_offset`.  Both return false on OOM or invalid range.           */
bool sol_text_buffer_insert_bytes(SolTextBuffer *tb,
                                   size_t byte_offset,
                                   const char *text, size_t len);
bool sol_text_buffer_delete_bytes(SolTextBuffer *tb,
                                   size_t byte_offset, size_t byte_count);

/* Set cursor to the given byte offset.  Clamps to rope length.        */
void sol_text_buffer_set_cursor_byte(SolTextBuffer *tb, size_t byte_offset);

/* Cursor motion. `dx` is codepoints (wraps across lines); `dy` is
   lines. `sticky_col` preserves the visual column across vertical
   motion (set true for Up/Down, false for Left/Right). */
void sol_text_buffer_move_cursor(SolTextBuffer *tb, int dx, int dy,
                                 bool sticky_col);

/* Move cursor to start / end of current line. */
void sol_text_buffer_move_line_start(SolTextBuffer *tb);
void sol_text_buffer_move_line_end  (SolTextBuffer *tb);

/* Position the cursor at (line, codepoint_column). Used by click. */
void sol_text_buffer_set_cursor_to(SolTextBuffer *tb,
                                   size_t line, size_t cp_col);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TEXT_BUFFER_H */
