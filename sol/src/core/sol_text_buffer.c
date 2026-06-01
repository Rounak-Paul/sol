// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_text_buffer.c — Rope-backed text-buffer module.
 *
 * Storage is a SolRope (B-tree of shared chunks). Cursor and scroll
 * metrics live in this module's state; edits go straight to the rope.
 *
 * Cursor is stored as a single byte offset into the rope. Display
 * (line, column) is derived on demand via sol_rope_line_of_byte /
 * sol_rope_byte_of_line — both O(log N).
 *
 * UTF-8 boundary navigation walks the rope through a small read window
 * around the cursor (we only ever need 1–4 bytes of context to find the
 * previous/next code-point boundary).
 */

#include "sol_text_buffer.h"

#include "sol_platform.h"
#include "sol_syntax.h"
#include "sol_syntax_highlight.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_event.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Maximum number of undoable records kept per buffer. */
#define TB_UNDO_MAX 512

/* One atomic change on the undo/redo stack. */
typedef struct {
    size_t byte_offset;
    char  *old_bytes;   /* bytes that were removed; NULL = pure insert */
    size_t old_len;
    char  *new_bytes;   /* bytes that were inserted; NULL = pure delete */
    size_t new_len;
    size_t cursor_before;
    size_t cursor_after;
    /* Selection state BEFORE this edit (for full undo restore). */
    size_t sel_anchor_before;
    bool   had_selection_before;
} TbEditRecord;

struct SolTextBuffer {
    SolRope              *rope;            /* owned                              */
    size_t                cursor_byte;     /* absolute byte offset into the rope */
    size_t                preferred_col_cp;
    int                   scroll_top_line; /* first visible line                 */
    char                 *source_path;     /* owned; NULL for unsaved/scratch    */

    SolEventBus          *events;
    SolBufferId           self_id;

    /* Syntax highlighter — NULL when no language matched the file extension. */
    SolSyntaxHighlighter *highlighter;

    /* Selection */
    size_t                sel_anchor_byte; /* non-moving end                     */
    bool                  has_selection;   /* true when a region is active       */

    /* Undo/redo — linear stack with watermark for redo.
       [0, undo_top)       = undoable records (most recent = undo_top-1)
       [undo_top, undo_end) = redoable records (oldest redo = undo_top) */
    TbEditRecord          undo_stack[TB_UNDO_MAX];
    int                   undo_top;
    int                   undo_end;
};

/* Publish sol.text.edited. `removed` and `inserted` are byte counts;
   `at` is the byte offset in the rope BEFORE the change took effect
   (so observers see a consistent pre-edit coordinate). */
static void tb_publish_edit(const SolTextBuffer *tb, size_t at,
                            size_t removed, size_t inserted)
{
    if (!tb || !tb->events) return;
    /* Re-parse so the highlight spans stay in sync with the rope. */
    if (tb->highlighter)
        sol_syntax_highlight_reparse(tb->highlighter, tb->rope);
    SolTextEditedPayload payload;
    payload.buffer_id      = tb->self_id;
    payload.byte_offset    = at;
    payload.removed_bytes  = removed;
    payload.inserted_bytes = inserted;
    sol_event_publish(tb->events, SOL_EVENT_TEXT_EDITED,
                       &payload, sizeof(payload), (void *)tb);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *tb_strdup(const char *s)
{
    if (!s) return NULL;
    const size_t n = strlen(s);
    char *o = (char *)malloc(n + 1u);
    if (!o) return NULL;
    memcpy(o, s, n + 1u);
    return o;
}

static const char *tb_basename(const char *path)
{
    if (!path || !*path) return "untitled";
    const char *base = sol_platform_basename(path);
    return (base && *base) ? base : path;
}

/* ---- UTF-8 codec -------------------------------------------------- */

static int tb_utf8_encode(uint32_t cp, uint8_t out[4])
{
    if (cp < 0x80u)   { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800u)  {
        out[0] = (uint8_t)(0xC0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0; /* surrogate */
        out[0] = (uint8_t)(0xE0u | (cp >> 12));
        out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (uint8_t)(0xF0u | (cp >> 18));
        out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* Byte length of the UTF-8 codepoint whose lead byte is `b`. Returns
   1 for malformed lead bytes so we always advance. */
static size_t tb_utf8_lead_len(uint8_t b)
{
    if ((b & 0x80u) == 0x00u) return 1u;
    if ((b & 0xE0u) == 0xC0u) return 2u;
    if ((b & 0xF0u) == 0xE0u) return 3u;
    if ((b & 0xF8u) == 0xF0u) return 4u;
    return 1u;
}

static bool tb_is_continuation(uint8_t b)
{
    return (b & 0xC0u) == 0x80u;
}

/* ---- Rope reading helpers ---------------------------------------- */

/* Read 1..max bytes starting at `byte_offset` into `out`. Clamped to
   the rope size. Returns bytes read. */
static size_t tb_rope_read_at(const SolRope *r, size_t at,
                              uint8_t *out, size_t max)
{
    return sol_rope_read(r, at, out, max);
}

/* Length of the UTF-8 codepoint starting at byte offset `at`. Returns
   0 at or past EOF. */
static size_t tb_cp_len_at(const SolRope *r, size_t at)
{
    if (at >= sol_rope_byte_len(r)) return 0u;
    uint8_t lead = 0;
    if (tb_rope_read_at(r, at, &lead, 1u) != 1u) return 0u;
    size_t len = tb_utf8_lead_len(lead);
    /* Clamp against EOF so partial trailing sequences still report 1. */
    const size_t remain = sol_rope_byte_len(r) - at;
    if (len > remain) len = remain ? remain : 1u;
    return len;
}

/* Length of the UTF-8 codepoint ending immediately before byte offset
   `at`. Returns 0 when at start of rope. */
static size_t tb_cp_len_before(const SolRope *r, size_t at)
{
    if (at == 0u) return 0u;
    /* Look back up to 4 bytes for a non-continuation lead. */
    const size_t window_start = at >= 4u ? at - 4u : 0u;
    uint8_t buf[4];
    const size_t n = tb_rope_read_at(r, window_start, buf, at - window_start);
    if (n == 0u) return 0u;
    size_t i = n;
    while (i > 0u && tb_is_continuation(buf[i - 1u])) --i;
    if (i == 0u) {
        /* All continuation bytes — malformed; treat as 1-byte step. */
        return 1u;
    }
    return n - (i - 1u);
}

/* Byte length of `line` excluding the trailing '\n'. */
static size_t tb_line_byte_len(const SolRope *r, size_t line)
{
    const size_t total_lines = sol_rope_line_count(r);
    const size_t start = sol_rope_byte_of_line(r, line);
    size_t end;
    if (line >= total_lines) {
        end = sol_rope_byte_len(r);
    } else {
        end = sol_rope_byte_of_line(r, line + 1u);
        /* end points to byte after the '\n' that ends `line` — back up
           one to exclude the newline from the length. */
        if (end > start) --end;
    }
    return end - start;
}

/* Codepoint count of the first `len` bytes of `line`. */
static size_t tb_line_cp_count(const SolRope *r, size_t line, size_t byte_len)
{
    const size_t start = sol_rope_byte_of_line(r, line);
    size_t off = 0u;
    size_t cp_count = 0u;
    while (off < byte_len) {
        uint8_t b = 0;
        if (tb_rope_read_at(r, start + off, &b, 1u) != 1u) break;
        const size_t step = tb_utf8_lead_len(b);
        off += step ? step : 1u;
        ++cp_count;
    }
    return cp_count;
}

/* Byte offset within `line` corresponding to codepoint column `cp_col`,
   clamped to the line's byte length. */
static size_t tb_line_byte_of_cp(const SolRope *r, size_t line, size_t cp_col)
{
    const size_t line_bytes = tb_line_byte_len(r, line);
    const size_t start      = sol_rope_byte_of_line(r, line);
    size_t off = 0u;
    size_t cp  = 0u;
    while (off < line_bytes && cp < cp_col) {
        uint8_t b = 0;
        if (tb_rope_read_at(r, start + off, &b, 1u) != 1u) break;
        const size_t step = tb_utf8_lead_len(b);
        off += step ? step : 1u;
        ++cp;
    }
    if (off > line_bytes) off = line_bytes;
    return off;
}

/* ------------------------------------------------------------------ */
/* Undo / redo helpers                                                 */
/* ------------------------------------------------------------------ */

/* Read `len` bytes from the rope at `at` into a malloc'd buffer.
   Returns NULL on OOM or len == 0. */
static char *tb_read_rope_bytes(const SolRope *r, size_t at, size_t len)
{
    if (len == 0u) return NULL;
    char *buf = (char *)malloc(len);
    if (!buf) return NULL;
    const size_t got = sol_rope_read(r, at, (uint8_t *)buf, len);
    if (got < len) memset(buf + got, 0, len - got);
    return buf;
}

/* Push one record onto the undo stack.  cursor_before / cursor_after
   are the cursor positions immediately before and after the edit.
   Consecutive single-codepoint inserts are grouped automatically. */
static void tb_push_undo(SolTextBuffer *tb,
                         size_t at,
                         const char *old_bytes, size_t old_len,
                         const char *new_bytes, size_t new_len,
                         size_t cursor_before,  size_t cursor_after)
{
    /* Clear all redo records. */
    for (int i = tb->undo_top; i < tb->undo_end; ++i) {
        free(tb->undo_stack[i].old_bytes);
        free(tb->undo_stack[i].new_bytes);
        tb->undo_stack[i].old_bytes = NULL;
        tb->undo_stack[i].new_bytes = NULL;
    }
    tb->undo_end = tb->undo_top;

    /* Group consecutive single-codepoint inserts (old_len == 0,
       new_len <= 4) into the previous record when it is also a pure
       insert ending right where this one starts. */
    if (tb->undo_top > 0 && old_len == 0u &&
        new_bytes && new_len > 0u && new_len <= 4u)
    {
        TbEditRecord *last = &tb->undo_stack[tb->undo_top - 1];
        if (last->old_len == 0u && last->new_bytes &&
            last->byte_offset + last->new_len == at &&
            last->new_len < 200u)
        {
            char *merged = (char *)realloc(last->new_bytes,
                                           last->new_len + new_len);
            if (merged) {
                memcpy(merged + last->new_len, new_bytes, new_len);
                last->new_bytes  = merged;
                last->new_len   += new_len;
                last->cursor_after = cursor_after;
                return;
            }
        }
    }

    /* Drop the oldest record when the stack is full. */
    if (tb->undo_top >= TB_UNDO_MAX) {
        free(tb->undo_stack[0].old_bytes);
        free(tb->undo_stack[0].new_bytes);
        memmove(tb->undo_stack, tb->undo_stack + 1,
                (size_t)(TB_UNDO_MAX - 1) * sizeof(TbEditRecord));
        tb->undo_top--;
        tb->undo_end--;
    }

    TbEditRecord *rec    = &tb->undo_stack[tb->undo_top];
    rec->byte_offset     = at;
    rec->cursor_before   = cursor_before;
    rec->cursor_after    = cursor_after;
    rec->sel_anchor_before   = tb->sel_anchor_byte;
    rec->had_selection_before = tb->has_selection;

    rec->old_bytes = NULL;
    rec->old_len   = 0u;
    if (old_bytes && old_len > 0u) {
        rec->old_bytes = (char *)malloc(old_len);
        if (rec->old_bytes) {
            memcpy(rec->old_bytes, old_bytes, old_len);
            rec->old_len = old_len;
        }
    }

    rec->new_bytes = NULL;
    rec->new_len   = 0u;
    if (new_bytes && new_len > 0u) {
        rec->new_bytes = (char *)malloc(new_len);
        if (rec->new_bytes) {
            memcpy(rec->new_bytes, new_bytes, new_len);
            rec->new_len = new_len;
        }
    }

    ++tb->undo_top;
    tb->undo_end = tb->undo_top;
}

/* ------------------------------------------------------------------ */
/* Word-character classification                                       */
/* ------------------------------------------------------------------ */

static bool tb_is_word_char(uint8_t b)
{
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || b == '_' || (b >= 0x80u);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void tb_destroy(void *state)
{
    SolTextBuffer *tb = (SolTextBuffer *)state;
    if (!tb) return;
    sol_syntax_highlight_destroy(tb->highlighter);
    if (tb->rope) sol_rope_destroy(tb->rope);
    free(tb->source_path);
    /* Free undo/redo string storage. */
    for (int i = 0; i < tb->undo_end; ++i) {
        free(tb->undo_stack[i].old_bytes);
        free(tb->undo_stack[i].new_bytes);
    }
    free(tb);
}

static SolTextBuffer *tb_create_from_rope(SolRope *rope, const char *source_path)
{
    if (!rope) return NULL;
    SolTextBuffer *tb = (SolTextBuffer *)calloc(1u, sizeof(SolTextBuffer));
    if (!tb) {
        sol_rope_destroy(rope);
        return NULL;
    }
    tb->rope = rope;
    if (source_path) {
        tb->source_path = tb_strdup(source_path);
        /* tb_strdup failure is non-fatal — we just can't dedupe. */
    }
    return tb;
}

static SolBufferId tb_register(SolBufferSystem *system, SolTextBuffer *tb,
                               const char *display_name,
                               SolBufferRenderFn render)
{
    SolBufferDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.name        = display_name;
    desc.kind        = SOL_BUFFER_KIND_TEXT;
    desc.state       = tb;
    desc.ops.destroy = tb_destroy;
    desc.ops.render  = render;
    const SolBufferId id = sol_buffer_create(system, &desc);
    if (id == 0u) {
        tb_destroy(tb);
    } else {
        tb->events  = sol_buffer_event_bus(system);
        tb->self_id = id;

        /* Attach syntax highlighter if a language is registered for this
         * file's extension.  Requires the global registry to be set
         * (via sol_syntax_set_global_registry) before files are opened. */
        if (tb->source_path) {
            SolSyntaxRegistry *reg = sol_syntax_get_global_registry();
            if (reg) {
                const void *lang =
                    sol_syntax_get_for_path(reg, tb->source_path);
                if (lang) {
                    tb->highlighter =
                        sol_syntax_highlight_create(lang);
                    if (tb->highlighter)
                        sol_syntax_highlight_reparse(
                            tb->highlighter, tb->rope);
                }
            }
        }
    }
    return id;
}

SolBufferId sol_text_buffer_open_empty(SolBufferSystem *system,
                                       const char *display_name,
                                       SolBufferRenderFn render)
{
    if (!system) return 0u;
    SolRope *rope = sol_rope_create();
    SolTextBuffer *tb = tb_create_from_rope(rope, NULL);
    if (!tb) return 0u;
    return tb_register(system, tb, display_name ? display_name : "untitled",
                       render);
}

SolBufferId sol_text_buffer_open_file(SolBufferSystem *system,
                                      const char *path,
                                      const char *display_name,
                                      SolBufferRenderFn render,
                                      const char **out_error)
{
    if (!system || !path) {
        if (out_error) *out_error = "null argument";
        return 0u;
    }
    const char *err = NULL;
    SolRope *rope = sol_rope_from_file(path, &err);
    if (!rope) {
        if (out_error) *out_error = err ? err : "failed to load file";
        return 0u;
    }
    SolTextBuffer *tb = tb_create_from_rope(rope, path);
    if (!tb) {
        if (out_error) *out_error = "out of memory";
        return 0u;
    }
    const SolBufferId id = tb_register(
        system, tb, display_name ? display_name : tb_basename(path), render);
    if (id == 0u && out_error) *out_error = "buffer system rejected the buffer";
    return id;
}

SolBufferId sol_text_buffer_open_string(SolBufferSystem *system,
                                        const char *display_name,
                                        const char *text, size_t len,
                                        const char *source_path,
                                        SolBufferRenderFn render)
{
    if (!system) return 0u;
    SolRope *rope = text && len > 0u
                        ? sol_rope_from_bytes((const uint8_t *)text, len)
                        : sol_rope_create();
    SolTextBuffer *tb = tb_create_from_rope(rope, source_path);
    if (!tb) return 0u;
    const char *name = display_name ? display_name
                                    : (source_path ? tb_basename(source_path)
                                                   : "untitled");
    return tb_register(system, tb, name, render);
}

/* ------------------------------------------------------------------ */
/* Find / accessors                                                    */
/* ------------------------------------------------------------------ */

SolSyntaxHighlighter *sol_text_buffer_highlighter(const SolTextBuffer *tb)
{
    return tb ? tb->highlighter : NULL;
}

void sol_text_buffer_invalidate_language(SolBufferSystem *system,
                                          const void      *language)
{
    if (!system || !language) return;
    const size_t total = sol_buffer_count(system);
    for (size_t i = 0u; i < total; ++i) {
        SolBuffer *buf = sol_buffer_get(system, sol_buffer_at(system, i));
        if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) continue;
        SolTextBuffer *tb = (SolTextBuffer *)sol_buffer_state(buf);
        if (!tb || !tb->highlighter) continue;
        if (sol_syntax_highlight_get_language(tb->highlighter) == language) {
            sol_syntax_highlight_destroy(tb->highlighter);
            tb->highlighter = NULL;
        }
    }
}

void sol_text_buffer_refresh_highlighters(SolBufferSystem *system)
{
    if (!system) return;
    SolSyntaxRegistry *reg = sol_syntax_get_global_registry();
    if (!reg) return;
    const size_t total = sol_buffer_count(system);
    for (size_t i = 0u; i < total; ++i) {
        SolBuffer *buf = sol_buffer_get(system, sol_buffer_at(system, i));
        if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) continue;
        SolTextBuffer *tb = (SolTextBuffer *)sol_buffer_state(buf);
        if (!tb || tb->highlighter || !tb->source_path) continue;
        const void *lang = sol_syntax_get_for_path(reg, tb->source_path);
        if (!lang) continue;
        tb->highlighter = sol_syntax_highlight_create(lang);
        if (tb->highlighter && tb->rope)
            sol_syntax_highlight_reparse(tb->highlighter, tb->rope);
    }
}

SolBufferId sol_text_buffer_find_by_path(SolBufferSystem *system, const char *path)
{
    if (!system || !path) return 0u;
    const size_t total = sol_buffer_count(system);
    for (size_t i = 0u; i < total; ++i) {
        const SolBufferId id = sol_buffer_at(system, i);
        if (id == 0u) continue;
        SolBuffer *buf = sol_buffer_get(system, id);
        if (!buf || sol_buffer_kind(buf) != SOL_BUFFER_KIND_TEXT) continue;
        const SolTextBuffer *tb = (const SolTextBuffer *)sol_buffer_state(buf);
        if (!tb || !tb->source_path) continue;
        if (strcmp(tb->source_path, path) == 0) return id;
    }
    return 0u;
}

SolTextBuffer *sol_text_buffer_state(SolBuffer *buffer)
{
    if (!buffer) return NULL;
    if (sol_buffer_kind(buffer) != SOL_BUFFER_KIND_TEXT) return NULL;
    return (SolTextBuffer *)sol_buffer_state(buffer);
}

SolTextBuffer *sol_text_buffer_active(SolBufferSystem *system)
{
    if (!system) return NULL;
    const SolBufferId id = sol_buffer_active_buffer(system);
    if (id == 0u) return NULL;
    SolBuffer *buf = sol_buffer_get(system, id);
    return sol_text_buffer_state(buf);
}

SolRope *sol_text_buffer_rope(SolBuffer *buffer)
{
    SolTextBuffer *tb = sol_text_buffer_state(buffer);
    return tb ? tb->rope : NULL;
}

const char *sol_text_buffer_source_path(const SolTextBuffer *tb)
{
    return tb ? tb->source_path : NULL;
}

/* ------------------------------------------------------------------ */
/* Cursor / scroll metrics                                             */
/* ------------------------------------------------------------------ */

size_t sol_text_buffer_cursor_byte(const SolTextBuffer *tb)
{
    return tb ? tb->cursor_byte : 0u;
}

size_t sol_text_buffer_cursor_line(const SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return 0u;
    return sol_rope_line_of_byte(tb->rope, tb->cursor_byte);
}

size_t sol_text_buffer_cursor_col(const SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return 0u;
    const size_t line = sol_rope_line_of_byte(tb->rope, tb->cursor_byte);
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    return tb->cursor_byte - line_start;
}

int sol_text_buffer_scroll_top(const SolTextBuffer *tb)
{
    return tb ? tb->scroll_top_line : 0;
}

void sol_text_buffer_set_scroll_top(SolTextBuffer *tb, int line)
{
    if (!tb) return;
    if (line < 0) line = 0;
    tb->scroll_top_line = line;
}

size_t sol_text_buffer_line_count(const SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return 1u;
    /* The rope's line_count is the number of '\n' bytes. A non-empty
       buffer with no trailing newline has the same number of visible
       lines as it has newlines + 1; a buffer ending in '\n' has one
       more line slot than rope_line_count would suggest only if there
       is content after the final '\n' — which there isn't by
       definition. So:
         displayed = max(1, line_count + (byte_len > line_starts_last)). */
    const size_t lc = sol_rope_line_count(tb->rope);
    const size_t bl = sol_rope_byte_len(tb->rope);
    if (bl == 0u) return 1u;
    /* Byte just past the final '\n' (or 0 if no newlines). */
    const size_t last_start = sol_rope_byte_of_line(tb->rope, lc);
    const bool has_trailing_partial = last_start < bl;
    return lc + (has_trailing_partial ? 1u : 0u);
}

size_t sol_text_buffer_line_len(const SolTextBuffer *tb, size_t line)
{
    if (!tb || !tb->rope) return 0u;
    if (line >= sol_text_buffer_line_count(tb)) return 0u;
    return tb_line_byte_len(tb->rope, line);
}

size_t sol_text_buffer_copy_line(const SolTextBuffer *tb, size_t line,
                                 char *out, size_t max)
{
    if (!tb || !tb->rope || !out || max == 0u) return 0u;
    const size_t line_bytes = sol_text_buffer_line_len(tb, line);
    size_t n = line_bytes;
    if (n > max - 1u) n = max - 1u;
    const size_t start = sol_rope_byte_of_line(tb->rope, line);
    const size_t got = sol_rope_read(tb->rope, start, (uint8_t *)out, n);
    out[got] = '\0';
    return got;
}

void sol_text_buffer_ensure_cursor_visible(SolTextBuffer *tb, int viewport)
{
    if (!tb || viewport <= 0) return;
    const int cur_line = (int)sol_text_buffer_cursor_line(tb);
    if (cur_line < tb->scroll_top_line) {
        tb->scroll_top_line = cur_line;
    } else if (cur_line >= tb->scroll_top_line + viewport) {
        tb->scroll_top_line = cur_line - viewport + 1;
    }
    if (tb->scroll_top_line < 0) tb->scroll_top_line = 0;
}

/* ------------------------------------------------------------------ */
/* Cursor mutation helpers                                             */
/* ------------------------------------------------------------------ */

static void tb_set_cursor_byte(SolTextBuffer *tb, size_t byte)
{
    const size_t total = sol_rope_byte_len(tb->rope);
    if (byte > total) byte = total;
    /* Snap onto a codepoint boundary by walking back through
       continuation bytes. */
    while (byte > 0u) {
        uint8_t b = 0;
        if (sol_rope_read(tb->rope, byte, &b, 1u) != 1u) break;
        if (!tb_is_continuation(b)) break;
        --byte;
    }
    tb->cursor_byte = byte;
}

static void tb_update_preferred_col(SolTextBuffer *tb)
{
    const size_t line = sol_rope_line_of_byte(tb->rope, tb->cursor_byte);
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t col_bytes  = tb->cursor_byte - line_start;
    tb->preferred_col_cp    = tb_line_cp_count(tb->rope, line, col_bytes);
}

/* ------------------------------------------------------------------ */
/* Edits                                                               */
/* ------------------------------------------------------------------ */

bool sol_text_buffer_insert_codepoint(SolTextBuffer *tb, uint32_t cp)
{
    if (!tb || !tb->rope) return false;
    /* Replace selection if active. */
    if (tb->has_selection) sol_text_buffer_delete_selection(tb);
    uint8_t enc[4];
    const int n = tb_utf8_encode(cp, enc);
    if (n <= 0) return false;
    const size_t at = tb->cursor_byte;
    if (!sol_rope_insert(tb->rope, at, enc, (size_t)n)) return false;
    tb->cursor_byte += (size_t)n;
    tb->preferred_col_cp += 1u;
    tb_push_undo(tb, at, NULL, 0u, (const char *)enc, (size_t)n,
                 at, tb->cursor_byte);
    tb_publish_edit(tb, at, 0u, (size_t)n);
    return true;
}

bool sol_text_buffer_insert_newline(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    if (tb->has_selection) sol_text_buffer_delete_selection(tb);
    const uint8_t nl = '\n';
    const size_t at = tb->cursor_byte;
    if (!sol_rope_insert(tb->rope, at, &nl, 1u)) return false;
    tb->cursor_byte += 1u;
    tb->preferred_col_cp = 0u;
    tb_push_undo(tb, at, NULL, 0u, "\n", 1u, at, tb->cursor_byte);
    tb_publish_edit(tb, at, 0u, 1u);
    return true;
}

bool sol_text_buffer_backspace(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    if (tb->has_selection) return sol_text_buffer_delete_selection(tb);
    if (tb->cursor_byte == 0u) return false;
    const size_t step = tb_cp_len_before(tb->rope, tb->cursor_byte);
    if (step == 0u) return false;
    const size_t at = tb->cursor_byte - step;
    uint8_t removed_lead = 0;
    tb_rope_read_at(tb->rope, at, &removed_lead, 1u);
    char *old = tb_read_rope_bytes(tb->rope, at, step);
    if (!sol_rope_remove(tb->rope, at, step)) { free(old); return false; }
    const size_t cursor_before = at + step;
    tb->cursor_byte = at;
    if (removed_lead == (uint8_t)'\n') {
        tb_update_preferred_col(tb);
    } else if (tb->preferred_col_cp > 0u) {
        tb->preferred_col_cp -= 1u;
    }
    tb_push_undo(tb, at, old, step, NULL, 0u, cursor_before, at);
    free(old);
    tb_publish_edit(tb, at, step, 0u);
    return true;
}

bool sol_text_buffer_delete_forward(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    if (tb->has_selection) return sol_text_buffer_delete_selection(tb);
    const size_t step = tb_cp_len_at(tb->rope, tb->cursor_byte);
    if (step == 0u) return false;
    const size_t at = tb->cursor_byte;
    uint8_t deleted_lead = 0;
    tb_rope_read_at(tb->rope, at, &deleted_lead, 1u);
    char *old = tb_read_rope_bytes(tb->rope, at, step);
    if (!sol_rope_remove(tb->rope, at, step)) { free(old); return false; }
    if (deleted_lead == (uint8_t)'\n') tb_update_preferred_col(tb);
    tb_push_undo(tb, at, old, step, NULL, 0u, at, at);
    free(old);
    tb_publish_edit(tb, at, step, 0u);
    return true;
}

/* ------------------------------------------------------------------ */
/* Raw (selection-neutral) motion helpers                              */
/* ------------------------------------------------------------------ */

static void tb_move_cursor_raw(SolTextBuffer *tb, int dx, int dy,
                                bool sticky_col)
{
    const size_t total = sol_rope_byte_len(tb->rope);
    if (dx != 0 && dy == 0) {
        if (dx > 0) {
            for (int i = 0; i < dx; ++i) {
                const size_t step = tb_cp_len_at(tb->rope, tb->cursor_byte);
                if (step == 0u || tb->cursor_byte + step > total) break;
                tb->cursor_byte += step;
            }
        } else {
            for (int i = 0; i < -dx; ++i) {
                const size_t step = tb_cp_len_before(tb->rope, tb->cursor_byte);
                if (step == 0u) break;
                tb->cursor_byte -= step;
            }
        }
        tb_update_preferred_col(tb);
        return;
    }
    if (dy != 0) {
        const size_t target_cp = sticky_col
            ? tb->preferred_col_cp
            : (sol_text_buffer_cursor_col(tb) == 0u ? 0u : tb->preferred_col_cp);
        long new_line = (long)sol_text_buffer_cursor_line(tb) + dy;
        const long total_lines = (long)sol_text_buffer_line_count(tb);
        if (new_line < 0) new_line = 0;
        if (new_line >= total_lines) new_line = total_lines - 1;
        const size_t line_start = sol_rope_byte_of_line(tb->rope, (size_t)new_line);
        const size_t col_bytes  = tb_line_byte_of_cp(tb->rope, (size_t)new_line, target_cp);
        tb->cursor_byte = line_start + col_bytes;
    }
}

static void tb_move_line_start_raw(SolTextBuffer *tb)
{
    const size_t line = sol_text_buffer_cursor_line(tb);
    tb->cursor_byte = sol_rope_byte_of_line(tb->rope, line);
    tb->preferred_col_cp = 0u;
}

static void tb_move_line_end_raw(SolTextBuffer *tb)
{
    const size_t line = sol_text_buffer_cursor_line(tb);
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t line_bytes = tb_line_byte_len(tb->rope, line);
    tb->cursor_byte = line_start + line_bytes;
    tb_update_preferred_col(tb);
}

static void tb_set_cursor_to_raw(SolTextBuffer *tb, size_t line, size_t cp_col)
{
    const size_t total_lines = sol_text_buffer_line_count(tb);
    if (line >= total_lines) line = total_lines ? total_lines - 1u : 0u;
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t col_bytes  = tb_line_byte_of_cp(tb->rope, line, cp_col);
    tb_set_cursor_byte(tb, line_start + col_bytes);
    tb->preferred_col_cp = cp_col;
}


void sol_text_buffer_move_cursor(SolTextBuffer *tb, int dx, int dy,
                                 bool sticky_col)
{
    if (!tb || !tb->rope) return;
    tb->has_selection = false;
    tb_move_cursor_raw(tb, dx, dy, sticky_col);
}

void sol_text_buffer_move_line_start(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return;
    tb->has_selection = false;
    tb_move_line_start_raw(tb);
}

void sol_text_buffer_move_line_end(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return;
    tb->has_selection = false;
    tb_move_line_end_raw(tb);
}

void sol_text_buffer_set_cursor_to(SolTextBuffer *tb, size_t line, size_t cp_col)
{
    if (!tb || !tb->rope) return;
    tb->has_selection = false;
    tb_set_cursor_to_raw(tb, line, cp_col);
}

/* ------------------------------------------------------------------ */
/* Raw byte-offset mutations (plugin / scripting use)                  */
/* ------------------------------------------------------------------ */

bool sol_text_buffer_insert_bytes(SolTextBuffer *tb,
                                   size_t byte_offset,
                                   const char *text, size_t len)
{
    if (!tb || !tb->rope || !text || len == 0u) return false;
    const size_t total = sol_rope_byte_len(tb->rope);
    if (byte_offset > total) return false;
    if (!sol_rope_insert(tb->rope, byte_offset,
                         (const uint8_t *)text, len)) {
        return false;
    }
    /* Adjust cursor if it sits at or after the insertion point. */
    if (tb->cursor_byte >= byte_offset) {
        tb->cursor_byte += len;
    }
    tb_publish_edit(tb, byte_offset, 0u, len);
    return true;
}

bool sol_text_buffer_delete_bytes(SolTextBuffer *tb,
                                   size_t byte_offset, size_t byte_count)
{
    if (!tb || !tb->rope || byte_count == 0u) return false;
    const size_t total = sol_rope_byte_len(tb->rope);
    if (byte_offset >= total) return false;
    if (byte_offset + byte_count > total)
        byte_count = total - byte_offset;
    if (!sol_rope_remove(tb->rope, byte_offset, byte_count)) return false;
    /* Clamp cursor into the shortened rope. */
    if (tb->cursor_byte > byte_offset) {
        if (tb->cursor_byte >= byte_offset + byte_count)
            tb->cursor_byte -= byte_count;
        else
            tb->cursor_byte = byte_offset;
    }
    tb_publish_edit(tb, byte_offset, byte_count, 0u);
    return true;
}

void sol_text_buffer_set_cursor_byte(SolTextBuffer *tb, size_t byte_offset)
{
    if (!tb || !tb->rope) return;
    const size_t total = sol_rope_byte_len(tb->rope);
    if (byte_offset > total) byte_offset = total;
    tb->has_selection = false;
    tb_set_cursor_byte(tb, byte_offset);
    tb_update_preferred_col(tb);
}

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

bool sol_text_buffer_has_selection(const SolTextBuffer *tb)
{
    return tb ? tb->has_selection : false;
}

void sol_text_buffer_selection_range(const SolTextBuffer *tb,
                                     size_t *out_start, size_t *out_end)
{
    if (!tb || !out_start || !out_end) return;
    if (!tb->has_selection) {
        *out_start = *out_end = tb->cursor_byte;
        return;
    }
    if (tb->cursor_byte <= tb->sel_anchor_byte) {
        *out_start = tb->cursor_byte;
        *out_end   = tb->sel_anchor_byte;
    } else {
        *out_start = tb->sel_anchor_byte;
        *out_end   = tb->cursor_byte;
    }
}

void sol_text_buffer_set_selection_anchor(SolTextBuffer *tb)
{
    if (!tb) return;
    tb->sel_anchor_byte = tb->cursor_byte;
    /* A zero-width selection is not visually active yet; has_selection
       stays false until the cursor moves while the anchor is held. */
}

void sol_text_buffer_clear_selection(SolTextBuffer *tb)
{
    if (!tb) return;
    tb->has_selection = false;
}

bool sol_text_buffer_delete_selection(SolTextBuffer *tb)
{
    if (!tb || !tb->rope || !tb->has_selection) return false;
    size_t sel_start, sel_end;
    sol_text_buffer_selection_range(tb, &sel_start, &sel_end);
    const size_t len = sel_end - sel_start;
    if (len == 0u) { tb->has_selection = false; return false; }
    const size_t cursor_before = tb->cursor_byte;
    char *old = tb_read_rope_bytes(tb->rope, sel_start, len);
    if (!sol_rope_remove(tb->rope, sel_start, len)) { free(old); return false; }
    tb->cursor_byte   = sel_start;
    tb->has_selection = false;
    tb_update_preferred_col(tb);
    tb_push_undo(tb, sel_start, old, len, NULL, 0u, cursor_before, sel_start);
    free(old);
    tb_publish_edit(tb, sel_start, len, 0u);
    return true;
}

void sol_text_buffer_select_all(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return;
    const size_t total = sol_rope_byte_len(tb->rope);
    tb->sel_anchor_byte = 0u;
    tb->cursor_byte     = total;
    tb->has_selection   = (total > 0u);
    tb_update_preferred_col(tb);
}

size_t sol_text_buffer_copy_selection_bytes(const SolTextBuffer *tb,
                                             char *out, size_t max)
{
    if (!tb || !tb->rope || !out || max == 0u || !tb->has_selection) return 0u;
    size_t sel_start, sel_end;
    sol_text_buffer_selection_range(tb, &sel_start, &sel_end);
    size_t len = sel_end - sel_start;
    if (len > max) len = max;
    return sol_rope_read(tb->rope, sel_start, (uint8_t *)out, len);
}

/* ------------------------------------------------------------------ */
/* Motion with optional selection extension                            */
/* ------------------------------------------------------------------ */

/* Helper: ensure the anchor is set if we are about to extend, or clear
   the selection if not extending. */
static void tb_prep_sel(SolTextBuffer *tb, bool extend)
{
    if (extend) {
        if (!tb->has_selection) {
            tb->sel_anchor_byte = tb->cursor_byte;
            tb->has_selection   = true;
        }
    } else {
        tb->has_selection = false;
    }
}

/* After moving: if cursor == anchor the selection collapsed — clear it. */
static void tb_finalize_sel(SolTextBuffer *tb, bool extend)
{
    if (extend && tb->cursor_byte == tb->sel_anchor_byte)
        tb->has_selection = false;
}

void sol_text_buffer_move_cursor_sel(SolTextBuffer *tb,
                                     int dx, int dy, bool sticky_col,
                                     bool extend_sel)
{
    if (!tb || !tb->rope) return;
    tb_prep_sel(tb, extend_sel);
    tb_move_cursor_raw(tb, dx, dy, sticky_col);
    tb_finalize_sel(tb, extend_sel);
}

void sol_text_buffer_move_line_start_sel(SolTextBuffer *tb, bool extend_sel)
{
    if (!tb || !tb->rope) return;
    tb_prep_sel(tb, extend_sel);
    tb_move_line_start_raw(tb);
    tb_finalize_sel(tb, extend_sel);
}

void sol_text_buffer_move_line_end_sel(SolTextBuffer *tb, bool extend_sel)
{
    if (!tb || !tb->rope) return;
    tb_prep_sel(tb, extend_sel);
    tb_move_line_end_raw(tb);
    tb_finalize_sel(tb, extend_sel);
}

void sol_text_buffer_set_cursor_to_sel(SolTextBuffer *tb,
                                       size_t line, size_t cp_col,
                                       bool extend_sel)
{
    if (!tb || !tb->rope) return;
    tb_prep_sel(tb, extend_sel);
    tb_set_cursor_to_raw(tb, line, cp_col);
    tb_finalize_sel(tb, extend_sel);
}

void sol_text_buffer_move_page(SolTextBuffer *tb, int dir,
                               bool extend_sel, int viewport_lines)
{
    if (!tb || !tb->rope || viewport_lines <= 0) return;
    tb_prep_sel(tb, extend_sel);
    tb_move_cursor_raw(tb, 0, dir * viewport_lines, true);
    sol_text_buffer_ensure_cursor_visible(tb, viewport_lines);
    tb_finalize_sel(tb, extend_sel);
}

/* ------------------------------------------------------------------ */
/* Word motion                                                         */
/* ------------------------------------------------------------------ */

void sol_text_buffer_move_word(SolTextBuffer *tb, int dir, bool extend_sel)
{
    if (!tb || !tb->rope) return;
    const size_t total = sol_rope_byte_len(tb->rope);
    tb_prep_sel(tb, extend_sel);

    size_t pos = tb->cursor_byte;
    uint8_t b = 0;

    if (dir > 0) {
        /* Skip current word chars (if any), then skip non-word chars. */
        while (pos < total) {
            if (sol_rope_read(tb->rope, pos, &b, 1u) != 1u) break;
            if (!tb_is_word_char(b)) break;
            pos += tb_utf8_lead_len(b);
        }
        while (pos < total) {
            if (sol_rope_read(tb->rope, pos, &b, 1u) != 1u) break;
            if (tb_is_word_char(b)) break;
            pos += tb_utf8_lead_len(b);
        }
    } else {
        /* Skip non-word chars backward, then word chars backward. */
        while (pos > 0u) {
            const size_t step = tb_cp_len_before(tb->rope, pos);
            if (step == 0u) break;
            if (sol_rope_read(tb->rope, pos - step, &b, 1u) != 1u) break;
            if (tb_is_word_char(b)) break;
            pos -= step;
        }
        while (pos > 0u) {
            const size_t step = tb_cp_len_before(tb->rope, pos);
            if (step == 0u) break;
            if (sol_rope_read(tb->rope, pos - step, &b, 1u) != 1u) break;
            if (!tb_is_word_char(b)) break;
            pos -= step;
        }
    }

    if (pos > total) pos = total;
    tb->cursor_byte = pos;
    tb_update_preferred_col(tb);
    tb_finalize_sel(tb, extend_sel);
}

bool sol_text_buffer_delete_word_back(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    if (tb->has_selection) return sol_text_buffer_delete_selection(tb);
    const size_t total = sol_rope_byte_len(tb->rope);
    const size_t start = tb->cursor_byte;
    if (start == 0u) return false;

    uint8_t b = 0;
    /* Find the start of the current word (go backward past any word chars). */
    size_t pos = start;
    /* If we're inside a word, go back to its start. */
    {
        const size_t step = tb_cp_len_before(tb->rope, pos);
        if (step > 0u && sol_rope_read(tb->rope, pos - step, &b, 1u) == 1u
                && tb_is_word_char(b)) {
            while (pos > 0u) {
                const size_t s = tb_cp_len_before(tb->rope, pos);
                if (s == 0u) break;
                if (sol_rope_read(tb->rope, pos - s, &b, 1u) != 1u) break;
                if (!tb_is_word_char(b)) break;
                pos -= s;
            }
        } else {
            /* Not in a word — skip non-word chars back, then skip word chars back. */
            while (pos > 0u) {
                const size_t s = tb_cp_len_before(tb->rope, pos);
                if (s == 0u) break;
                if (sol_rope_read(tb->rope, pos - s, &b, 1u) != 1u) break;
                if (tb_is_word_char(b)) break;
                pos -= s;
            }
            while (pos > 0u) {
                const size_t s = tb_cp_len_before(tb->rope, pos);
                if (s == 0u) break;
                if (sol_rope_read(tb->rope, pos - s, &b, 1u) != 1u) break;
                if (!tb_is_word_char(b)) break;
                pos -= s;
            }
        }
    }
    /* Now extend end forward past any remaining word chars (other half of word). */
    size_t end = start;
    while (end < total) {
        if (sol_rope_read(tb->rope, end, &b, 1u) != 1u) break;
        if (!tb_is_word_char(b)) break;
        end += tb_utf8_lead_len(b);
    }
    if (pos == end) return false;
    const size_t len = end - pos;
    char *old = tb_read_rope_bytes(tb->rope, pos, len);
    if (!sol_rope_remove(tb->rope, pos, len)) { free(old); return false; }
    tb->cursor_byte = pos;
    tb_update_preferred_col(tb);
    tb_push_undo(tb, pos, old, len, NULL, 0u, start, pos);
    free(old);
    tb_publish_edit(tb, pos, len, 0u);
    return true;
}

bool sol_text_buffer_delete_word_forward(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    if (tb->has_selection) return sol_text_buffer_delete_selection(tb);
    const size_t cursor = tb->cursor_byte;
    const size_t total  = sol_rope_byte_len(tb->rope);
    if (cursor == total) return false;

    uint8_t b = 0;
    /* Snap to the start of the current word (go backward if mid-word). */
    size_t word_start = cursor;
    while (word_start > 0u) {
        const size_t s = tb_cp_len_before(tb->rope, word_start);
        if (s == 0u) break;
        if (sol_rope_read(tb->rope, word_start - s, &b, 1u) != 1u) break;
        if (!tb_is_word_char(b)) break;
        word_start -= s;
    }

    /* Walk forward from cursor to end of word. */
    size_t word_end = cursor;
    /* If we're on a non-word char, skip those first to reach the next word. */
    if (word_end < total) {
        if (sol_rope_read(tb->rope, word_end, &b, 1u) == 1u && !tb_is_word_char(b)) {
            while (word_end < total) {
                if (sol_rope_read(tb->rope, word_end, &b, 1u) != 1u) break;
                if (tb_is_word_char(b)) break;
                word_end += tb_utf8_lead_len(b);
            }
            /* Then skip the word chars of that next word too. */
        }
    }
    while (word_end < total) {
        if (sol_rope_read(tb->rope, word_end, &b, 1u) != 1u) break;
        if (!tb_is_word_char(b)) break;
        word_end += tb_utf8_lead_len(b);
    }

    const size_t del_start = word_start;
    const size_t del_end   = word_end;
    if (del_start == del_end) return false;
    const size_t len = del_end - del_start;
    char *old = tb_read_rope_bytes(tb->rope, del_start, len);
    if (!sol_rope_remove(tb->rope, del_start, len)) { free(old); return false; }
    tb->cursor_byte = del_start;
    tb_update_preferred_col(tb);
    tb_push_undo(tb, del_start, old, len, NULL, 0u, cursor, del_start);
    free(old);
    tb_publish_edit(tb, del_start, len, 0u);
    return true;
}

/* ------------------------------------------------------------------ */
/* Line operations                                                     */
/* ------------------------------------------------------------------ */

bool sol_text_buffer_duplicate_line(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    const size_t line       = sol_text_buffer_cursor_line(tb);
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t line_bytes = tb_line_byte_len(tb->rope, line);
    /* Build: '\n' + line_content — inserted before the trailing newline. */
    const size_t insert_len = 1u + line_bytes;
    char *ins = (char *)malloc(insert_len);
    if (!ins) return false;
    ins[0] = '\n';
    if (line_bytes > 0u)
        sol_rope_read(tb->rope, line_start, (uint8_t *)(ins + 1u), line_bytes);
    const size_t insert_at    = line_start + line_bytes; /* before the '\n' */
    const size_t cursor_before = tb->cursor_byte;
    if (!sol_rope_insert(tb->rope, insert_at,
                         (const uint8_t *)ins, insert_len)) {
        free(ins); return false;
    }
    /* Move cursor to same column on the duplicated line below. */
    const size_t cursor_col = tb->cursor_byte - line_start;
    const size_t clamped    = cursor_col <= line_bytes ? cursor_col : line_bytes;
    tb->cursor_byte  = insert_at + 1u + clamped;
    tb->has_selection = false;
    tb_update_preferred_col(tb);
    tb_push_undo(tb, insert_at, NULL, 0u, ins, insert_len,
                 cursor_before, tb->cursor_byte);
    free(ins);
    tb_publish_edit(tb, insert_at, 0u, insert_len);
    return true;
}

bool sol_text_buffer_delete_line(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    const size_t line       = sol_text_buffer_cursor_line(tb);
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t line_bytes = tb_line_byte_len(tb->rope, line);
    const size_t total      = sol_rope_byte_len(tb->rope);
    /* Include the trailing newline in the deletion range. */
    size_t del_len = line_bytes;
    const size_t nl_at = line_start + line_bytes;
    if (nl_at < total) {
        uint8_t b = 0;
        tb_rope_read_at(tb->rope, nl_at, &b, 1u);
        if (b == '\n') del_len++;
    }
    if (del_len == 0u) return false;
    const size_t cursor_before = tb->cursor_byte;
    char *old = tb_read_rope_bytes(tb->rope, line_start, del_len);
    if (!sol_rope_remove(tb->rope, line_start, del_len)) {
        free(old); return false;
    }
    const size_t new_total  = sol_rope_byte_len(tb->rope);
    tb->cursor_byte   = line_start <= new_total ? line_start : new_total;
    tb->has_selection = false;
    tb_update_preferred_col(tb);
    tb_push_undo(tb, line_start, old, del_len, NULL, 0u,
                 cursor_before, tb->cursor_byte);
    free(old);
    tb_publish_edit(tb, line_start, del_len, 0u);
    return true;
}

/* ------------------------------------------------------------------ */
/* Undo / redo                                                         */
/* ------------------------------------------------------------------ */

bool sol_text_buffer_can_undo(const SolTextBuffer *tb)
{
    return tb && tb->undo_top > 0;
}

bool sol_text_buffer_can_redo(const SolTextBuffer *tb)
{
    return tb && tb->undo_top < tb->undo_end;
}

bool sol_text_buffer_undo(SolTextBuffer *tb)
{
    if (!tb || !tb->rope || tb->undo_top <= 0) return false;
    --tb->undo_top;
    const TbEditRecord *rec = &tb->undo_stack[tb->undo_top];
    const size_t at = rec->byte_offset;
    /* Remove what was inserted. */
    if (rec->new_len > 0u)
        sol_rope_remove(tb->rope, at, rec->new_len);
    /* Re-insert what was there before. */
    if (rec->old_len > 0u && rec->old_bytes)
        sol_rope_insert(tb->rope, at,
                        (const uint8_t *)rec->old_bytes, rec->old_len);
    /* Restore cursor and selection. */
    tb_set_cursor_byte(tb, rec->cursor_before);
    tb_update_preferred_col(tb);
    tb->sel_anchor_byte = rec->sel_anchor_before;
    tb->has_selection   = rec->had_selection_before;
    tb_publish_edit(tb, at, rec->new_len, rec->old_len);
    return true;
}

bool sol_text_buffer_redo(SolTextBuffer *tb)
{
    if (!tb || !tb->rope || tb->undo_top >= tb->undo_end) return false;
    const TbEditRecord *rec = &tb->undo_stack[tb->undo_top];
    ++tb->undo_top;
    const size_t at = rec->byte_offset;
    /* Remove what was there before. */
    if (rec->old_len > 0u)
        sol_rope_remove(tb->rope, at, rec->old_len);
    /* Re-insert what was inserted. */
    if (rec->new_len > 0u && rec->new_bytes)
        sol_rope_insert(tb->rope, at,
                        (const uint8_t *)rec->new_bytes, rec->new_len);
    tb_set_cursor_byte(tb, rec->cursor_after);
    tb_update_preferred_col(tb);
    tb->has_selection = false;
    tb_publish_edit(tb, at, rec->old_len, rec->new_len);
    return true;
}
