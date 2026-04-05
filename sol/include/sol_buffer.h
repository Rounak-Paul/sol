#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   MODAL EDITING MODES  (Neovim-style)
   ============================================================ */

typedef enum Sol_Mode {
    SOL_MODE_NORMAL = 0, /* navigation / command mode    */
    SOL_MODE_INSERT,     /* text insertion mode          */
    SOL_MODE_VISUAL,     /* visual (selection) mode      */
} Sol_Mode;

/* ============================================================
   OPAQUE BUFFER HANDLE
   ============================================================ */

typedef struct Sol_Buffer Sol_Buffer;

/* ============================================================
   LIFECYCLE
   ============================================================ */

/* Create an empty unnamed buffer with one empty line. */
Sol_Buffer *sol_buffer_create(void);

/* Open a file from disk.  Returns NULL if the file cannot be read.
   The buffer is considered clean (not dirty) after a successful open. */
Sol_Buffer *sol_buffer_open(const char *path);

/* Free all resources.  Emits SOL_EVENT_BUFFER_CLOSE before freeing. */
void sol_buffer_destroy(Sol_Buffer *buf);

/* ============================================================
   PROPERTIES
   ============================================================ */

/* Unique, monotonically-increasing ID assigned at creation time.
   IDs are never reused; safe to use as stable keys. */
uint32_t    sol_buffer_id(const Sol_Buffer *buf);

/* The file path associated with this buffer, or NULL for unnamed buffers. */
const char *sol_buffer_path(const Sol_Buffer *buf);

/* True if the buffer has unsaved edits. */
bool        sol_buffer_is_dirty(const Sol_Buffer *buf);

/* Current modal editing mode. */
Sol_Mode    sol_buffer_mode(const Sol_Buffer *buf);
void        sol_buffer_set_mode(Sol_Buffer *buf, Sol_Mode mode);

/* ============================================================
   CONTENT ACCESS
   ============================================================ */

/* Number of lines.  Always >= 1 (an empty buffer has one empty line). */
uint32_t    sol_buffer_line_count(const Sol_Buffer *buf);

/* Pointer to the null-terminated text of the given line.
   `out_len` (if non-NULL) receives the byte length excluding the null.
   The pointer is valid until the next modification to that line.
   Returns NULL if `line` is out of range. */
const char *sol_buffer_line(const Sol_Buffer *buf, uint32_t line, uint32_t *out_len);

/* Total byte count across all lines (excluding per-line null terminators). */
uint32_t    sol_buffer_total_bytes(const Sol_Buffer *buf);

/* ============================================================
   EDITING
   ============================================================ */

/* Insert `len` bytes of `text` at position (line, col).
   Embedded newlines ('\n') split lines.  CRLF ("\r\n") is normalised to LF.
   `col` is clamped to the line length.
   Emits SOL_EVENT_BUFFER_CHANGE.  Marks the buffer dirty. */
void sol_buffer_insert(Sol_Buffer *buf,
                       uint32_t line, uint32_t col,
                       const char *text, uint32_t len);

/* Delete the range [line1:col1, line2:col2).
   If line1==line2, deletes bytes col1..col2 within that line.
   If line1 < line2, the lines are merged across the range.
   All coordinates are clamped to valid bounds.
   Emits SOL_EVENT_BUFFER_CHANGE.  Marks the buffer dirty. */
void sol_buffer_delete(Sol_Buffer *buf,
                       uint32_t line1, uint32_t col1,
                       uint32_t line2, uint32_t col2);

/* ============================================================
   CURSOR
   ============================================================ */

void sol_buffer_cursor_get(const Sol_Buffer *buf,
                           uint32_t *out_line, uint32_t *out_col);

/* Sets the cursor; coordinates are clamped to valid bounds.
   Emits SOL_EVENT_CURSOR_MOVE. */
void sol_buffer_cursor_set(Sol_Buffer *buf, uint32_t line, uint32_t col);

/* Move cursor by `dl` lines and `dc` byte columns (negative = backwards).
   Clamps at buffer boundaries.  Emits SOL_EVENT_CURSOR_MOVE. */
void sol_buffer_cursor_move(Sol_Buffer *buf, int dl, int dc);

/* ============================================================
   SELECTION
   ============================================================ */

bool sol_buffer_has_selection(const Sol_Buffer *buf);

void sol_buffer_selection_get(const Sol_Buffer *buf,
                               uint32_t *anchor_line, uint32_t *anchor_col,
                               uint32_t *active_line, uint32_t *active_col);

/* Clear any active selection. */
void sol_buffer_selection_clear(Sol_Buffer *buf);

/* ============================================================
   I / O
   ============================================================ */

/* Write to the buffer's current path.  Returns false on error. */
bool sol_buffer_save(Sol_Buffer *buf);

/* Write to a new path, update the buffer's path, mark clean.
   Returns false on error. */
bool sol_buffer_save_as(Sol_Buffer *buf, const char *path);

/* ============================================================
   GLOBAL REGISTRY
   ============================================================ */

/* Initialise the buffer registry.  Call once at startup. */
void sol_buffers_init(void);

/* Destroy all live buffers and free the registry. */
void sol_buffers_shutdown(void);

/* Fetch a buffer by its ID.  Returns NULL if not found. */
Sol_Buffer *sol_buffer_get_by_id(uint32_t id);

#ifdef __cplusplus
}
#endif
