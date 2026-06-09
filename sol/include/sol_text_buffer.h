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

/*
 * Open an empty text buffer.
 *
 * system        The buffer system to register the buffer with.
 * display_name  Name shown in tab strips and window titles.
 * render        Render callback; NULL means the buffer is invisible (useful for tests).
 * Returns       Buffer id, or 0 on failure.
 */
SolBufferId sol_text_buffer_open_empty(SolBufferSystem *system,
                                       const char *display_name,
                                       SolBufferRenderFn render);

/*
 * Open a file-backed text buffer via memory mapping (zero-copy until first edit).
 *
 * system        The buffer system.
 * path          Absolute path of the file to open.
 * display_name  Name shown in tabs.
 * render        Render callback; see sol_text_buffer_open_empty.
 * out_error     If non-NULL, receives a static error string on failure.
 * Returns       Buffer id, or 0 on failure.
 */
SolBufferId sol_text_buffer_open_file(SolBufferSystem *system,
                                      const char *path,
                                      const char *display_name,
                                      SolBufferRenderFn render,
                                      const char **out_error);

/*
 * Open a text buffer pre-populated from an in-memory string.
 *
 * system        The buffer system.
 * display_name  Name shown in tabs.
 * text          Initial content, or NULL for an empty buffer.
 * len           Byte length of text; 0 when text is NULL.
 * source_path   Optional path used for open-by-path deduplication; may be NULL.
 * render        Render callback.
 * Returns       Buffer id, or 0 on failure.
 */
SolBufferId sol_text_buffer_open_string(SolBufferSystem *system,
                                        const char *display_name,
                                        const char *text, size_t len,
                                        const char *source_path,
                                        SolBufferRenderFn render);

/*
 * Find an open text buffer whose source_path matches path (exact strcmp).
 *
 * system  The buffer system.
 * path    Absolute path to search for.
 * Returns The matching buffer id, or 0 when not found.
 */
SolBufferId sol_text_buffer_find_by_path(SolBufferSystem *system,
                                         const char *path);

/* ---- Accessors ---------------------------------------------------- */

/* Returns the mutable editor state attached to a TEXT buffer, or NULL for non-text. */
SolTextBuffer *sol_text_buffer_state(SolBuffer *buffer);

/* Returns the active text buffer's editor state, or NULL if nothing text-kind is focused. */
SolTextBuffer *sol_text_buffer_active(SolBufferSystem *system);

/* Returns a borrowed pointer to the underlying rope, or NULL for non-text buffers. */
SolRope *sol_text_buffer_rope(SolBuffer *buffer);

/* Returns the source path registered with the text buffer, or NULL if none. */
const char *sol_text_buffer_source_path(const SolTextBuffer *tb);

/* Returns the syntax highlighter attached to this buffer, or NULL when no language was matched. */
typedef struct SolSyntaxHighlighter SolSyntaxHighlighter;
SolSyntaxHighlighter *sol_text_buffer_highlighter(const SolTextBuffer *tb);

/*
 * Destroy and null out the highlighter for every text buffer using a language.
 *
 * Must be called before the library that owns language is unloaded.
 * No-op when language is NULL.
 *
 * system    The buffer system.
 * language  const TSLanguage* cast to const void* identifying the language.
 */
void sol_text_buffer_invalidate_language(SolBufferSystem *system,
                                          const void      *language);

/* Walk all TEXT buffers; for any buffer whose source_path has a
 * registered language in the global syntax registry and no current
 * highlighter, create and attach a fresh one.  Call this after a
 * language plugin finishes loading so already-open files gain
 * highlighting without requiring a close-reopen cycle.              */
void sol_text_buffer_refresh_highlighters(SolBufferSystem *system);

/* ---- Cursor / scroll metrics ------------------------------------- */

/* Returns the cursor position as a byte offset into the rope. */
size_t sol_text_buffer_cursor_byte (const SolTextBuffer *tb);

/* Returns the zero-based line index of the cursor. */
size_t sol_text_buffer_cursor_line (const SolTextBuffer *tb);

/* Returns the cursor's byte offset within its line (0-based). */
size_t sol_text_buffer_cursor_col  (const SolTextBuffer *tb);

/* Returns the zero-based index of the first visible line in the viewport. */
int  sol_text_buffer_scroll_top    (const SolTextBuffer *tb);

/* Set the first visible line in the viewport to line. */
void sol_text_buffer_set_scroll_top(SolTextBuffer *tb, int line);

/*
 * Returns the first visible visual column in the text viewport.
 *
 * Columns use the renderer's monospace/tab-expansion metrics rather than bytes.
 */
int  sol_text_buffer_scroll_left    (const SolTextBuffer *tb);

/* Set the first visible visual column in the text viewport to col. */
void sol_text_buffer_set_scroll_left(SolTextBuffer *tb, int col);

/*
 * Adjust scroll_top so the cursor sits within the visible viewport.
 *
 * tb              The text buffer.
 * viewport_lines  Height of the visible area in lines.
 */
void sol_text_buffer_ensure_cursor_visible(SolTextBuffer *tb,
                                           int viewport_lines);

/*
 * Adjust both scroll_top and scroll_left so the cursor is fully visible.
 *
 * tb              The text buffer.
 * viewport_lines  Visible height in lines.
 * viewport_cols   Visible width in visual columns.
 */
void sol_text_buffer_ensure_cursor_visible_2d(SolTextBuffer *tb,
                                              int viewport_lines,
                                              int viewport_cols);

/*
 * Returns the total logical line count (always >= 1; empty buffers have one line).
 */
size_t sol_text_buffer_line_count(const SolTextBuffer *tb);

/*
 * Copy a line's bytes into a caller-supplied buffer, NUL-terminating.
 *
 * tb    The text buffer.
 * line  Zero-based line index.
 * out   Destination buffer.
 * max   Total bytes available in out (including NUL).
 * Returns  Byte count copied, excluding the NUL terminator.
 */
size_t sol_text_buffer_copy_line(const SolTextBuffer *tb, size_t line,
                                 char *out, size_t max);

/*
 * Returns the byte length of a line, excluding its trailing newline.
 *
 * tb    The text buffer.
 * line  Zero-based line index.
 */
size_t sol_text_buffer_line_len(const SolTextBuffer *tb, size_t line);

/* ---- Edits -------------------------------------------------------- */

/* Insert a UTF-32 codepoint at the cursor position. Returns false on OOM. */
bool sol_text_buffer_insert_codepoint(SolTextBuffer *tb, uint32_t cp);

/* Insert a newline at the cursor position. Returns false on OOM. */
bool sol_text_buffer_insert_newline  (SolTextBuffer *tb);

/* Delete the codepoint immediately before the cursor. Returns false when at start. */
bool sol_text_buffer_backspace       (SolTextBuffer *tb);

/* Delete the codepoint immediately after the cursor. Returns false when at end. */
bool sol_text_buffer_delete_forward  (SolTextBuffer *tb);

/*
 * Insert bytes at an arbitrary byte offset without moving the cursor.
 *
 * Fires SOL_EVENT_TEXT_EDITED so plugins and the UI stay in sync.
 *
 * tb           The text buffer.
 * byte_offset  Insertion point in the rope.
 * text         Bytes to insert.
 * len          Number of bytes to insert.
 * Returns      false on OOM or invalid offset.
 */
bool sol_text_buffer_insert_bytes(SolTextBuffer *tb,
                                   size_t byte_offset,
                                   const char *text, size_t len);

/*
 * Delete bytes at an arbitrary byte offset without moving the cursor.
 *
 * Fires SOL_EVENT_TEXT_EDITED so plugins and the UI stay in sync.
 *
 * tb           The text buffer.
 * byte_offset  Start of the region to delete.
 * byte_count   Number of bytes to remove.
 * Returns      false on OOM or invalid range.
 */
bool sol_text_buffer_delete_bytes(SolTextBuffer *tb,
                                   size_t byte_offset, size_t byte_count);

/*
 * Move the cursor to an absolute byte offset.
 *
 * tb           The text buffer.
 * byte_offset  Target offset; clamped to rope length.
 */
void sol_text_buffer_set_cursor_byte(SolTextBuffer *tb, size_t byte_offset);

/*
 * Move the cursor by a relative offset, clearing any active selection.
 *
 * tb          The text buffer.
 * dx          Codepoint delta (wraps across line boundaries).
 * dy          Line delta.
 * sticky_col  When true, preserve the preferred visual column across vertical
 *             motion (use for Up/Down; false for Left/Right).
 */
void sol_text_buffer_move_cursor(SolTextBuffer *tb, int dx, int dy,
                                 bool sticky_col);

/* Move the cursor to the start of the current line, clearing selection. */
void sol_text_buffer_move_line_start(SolTextBuffer *tb);

/* Move the cursor to the end of the current line, clearing selection. */
void sol_text_buffer_move_line_end  (SolTextBuffer *tb);

/*
 * Position the cursor at a specific line and codepoint column.
 *
 * tb      The text buffer.
 * line    Zero-based target line index.
 * cp_col  Zero-based codepoint column within the line.
 */
void sol_text_buffer_set_cursor_to(SolTextBuffer *tb,
                                   size_t line, size_t cp_col);

/* ---- Selection ---------------------------------------------------- */

/* Returns true when a non-empty selection is active. */
bool sol_text_buffer_has_selection(const SolTextBuffer *tb);

/*
 * Return the selected byte range as [*out_start, *out_end).
 *
 * out_start is always <= out_end. When no selection is active both are
 * set to the cursor position.
 *
 * tb         The text buffer.
 * out_start  Receives the inclusive start byte offset.
 * out_end    Receives the exclusive end byte offset.
 */
void sol_text_buffer_selection_range(const SolTextBuffer *tb,
                                     size_t *out_start,
                                     size_t *out_end);

/*
 * Set the selection anchor at the current cursor position.
 *
 * A zero-width selection becomes visible the moment the cursor moves
 * away while the anchor is held.
 */
void sol_text_buffer_set_selection_anchor(SolTextBuffer *tb);

/* Drop the active selection without moving the cursor. */
void sol_text_buffer_clear_selection(SolTextBuffer *tb);

/*
 * Delete the selected region and place the cursor at the region start.
 *
 * Returns false when no selection is active or deletion fails.
 */
bool sol_text_buffer_delete_selection(SolTextBuffer *tb);

/* Select the entire buffer content. */
void sol_text_buffer_select_all(SolTextBuffer *tb);

/*
 * Copy the selected bytes into a caller-supplied buffer (no NUL appended).
 *
 * tb   The text buffer.
 * out  Destination buffer.
 * max  Maximum bytes to copy.
 * Returns  Bytes copied; 0 if no selection is active.
 */
size_t sol_text_buffer_copy_selection_bytes(const SolTextBuffer *tb,
                                             char *out, size_t max);

/* ---- Motion with optional selection extension --------------------- */

/*
 * Move the cursor with optional selection extension.
 *
 * When extend_sel is true, sets the anchor on the first call and extends the
 * selection on subsequent ones. When false, clears the selection.
 *
 * tb          The text buffer.
 * dx          Codepoint delta.
 * dy          Line delta.
 * sticky_col  Preserve preferred column across vertical motion.
 * extend_sel  true to extend the selection; false to clear it.
 */
void sol_text_buffer_move_cursor_sel(SolTextBuffer *tb,
                                     int dx, int dy, bool sticky_col,
                                     bool extend_sel);

/* Move to line start, extending or clearing selection according to extend_sel. */
void sol_text_buffer_move_line_start_sel(SolTextBuffer *tb, bool extend_sel);

/* Move to line end, extending or clearing selection according to extend_sel. */
void sol_text_buffer_move_line_end_sel  (SolTextBuffer *tb, bool extend_sel);

/*
 * Position the cursor, extending or clearing the selection.
 *
 * tb          The text buffer.
 * line        Zero-based target line.
 * cp_col      Zero-based codepoint column.
 * extend_sel  true to extend the selection; false to clear it.
 */
void sol_text_buffer_set_cursor_to_sel(SolTextBuffer *tb,
                                       size_t line, size_t cp_col,
                                       bool extend_sel);

/*
 * Move the cursor one page in the given direction.
 *
 * tb              The text buffer.
 * dir             Positive for page down, negative for page up.
 * extend_sel      true to extend the selection; false to clear it.
 * viewport_lines  Visible height in lines; determines the page size.
 */
void sol_text_buffer_move_page(SolTextBuffer *tb, int dir,
                               bool extend_sel, int viewport_lines);

/* ---- Word motion -------------------------------------------------- */

/*
 * Move the cursor one word in the given direction.
 *
 * tb          The text buffer.
 * dir         +1 for next word, -1 for previous word.
 * extend_sel  true to extend the selection; false to clear it.
 */
void sol_text_buffer_move_word(SolTextBuffer *tb, int dir, bool extend_sel);

/*
 * Delete from the cursor back to the start of the current word.
 *
 * If a selection is active, deletes the selection instead.
 * Returns false on failure.
 */
bool sol_text_buffer_delete_word_back   (SolTextBuffer *tb);

/*
 * Delete from the cursor forward to the end of the current word.
 *
 * If a selection is active, deletes the selection instead.
 * Returns false on failure.
 */
bool sol_text_buffer_delete_word_forward(SolTextBuffer *tb);

/* ---- Line operations ---------------------------------------------- */

/*
 * Insert a copy of the current line immediately below it.
 *
 * The cursor moves to the new line at the same codepoint column.
 * Returns false on OOM.
 */
bool sol_text_buffer_duplicate_line(SolTextBuffer *tb);

/*
 * Delete the entire current line including its trailing newline.
 *
 * Returns false on failure.
 */
bool sol_text_buffer_delete_line(SolTextBuffer *tb);

/* ---- Undo / redo -------------------------------------------------- */

/* Undo the most recent edit. Returns false when there is nothing to undo. */
bool sol_text_buffer_undo    (SolTextBuffer *tb);

/* Redo the most recently undone edit. Returns false when there is nothing to redo. */
bool sol_text_buffer_redo    (SolTextBuffer *tb);

/* Returns true if there are edits available to undo. */
bool sol_text_buffer_can_undo(const SolTextBuffer *tb);

/* Returns true if there are edits available to redo. */
bool sol_text_buffer_can_redo(const SolTextBuffer *tb);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TEXT_BUFFER_H */
