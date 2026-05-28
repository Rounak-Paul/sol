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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sol_event.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

struct SolTextBuffer {
    SolRope *rope;            /* owned                              */
    size_t   cursor_byte;     /* absolute byte offset into the rope */
    /* Sticky column for vertical motion, in codepoints from the
       start of the line. We track codepoints (not bytes) so the
       cursor visually lines up across lines containing multibyte
       glyphs. */
    size_t   preferred_col_cp;
    int      scroll_top_line; /* first visible line                 */
    char    *source_path;     /* owned; NULL for unsaved/scratch    */

    /* Event-bus wiring, filled in by tb_register() once the
       SolBufferSystem has minted an id. Both may be zero/NULL when
       the buffer is detached from a system. */
    SolEventBus *events;
    SolBufferId  self_id;
};

/* Publish sol.text.edited. `removed` and `inserted` are byte counts;
   `at` is the byte offset in the rope BEFORE the change took effect
   (so observers see a consistent pre-edit coordinate). */
static void tb_publish_edit(const SolTextBuffer *tb, size_t at,
                            size_t removed, size_t inserted)
{
    if (!tb || !tb->events) return;
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
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
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
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void tb_destroy(void *state)
{
    SolTextBuffer *tb = (SolTextBuffer *)state;
    if (!tb) return;
    if (tb->rope) sol_rope_destroy(tb->rope);
    free(tb->source_path);
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
        /* Wire the bus so subsequent edits can publish text events.
           Pulled from the buffer system so callers don't have to
           plumb it through every open_* entry point. */
        tb->events  = sol_buffer_event_bus(system);
        tb->self_id = id;
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
    uint8_t enc[4];
    const int n = tb_utf8_encode(cp, enc);
    if (n <= 0) return false;
    const size_t at = tb->cursor_byte;
    if (!sol_rope_insert(tb->rope, at, enc, (size_t)n)) {
        return false;
    }
    tb->cursor_byte += (size_t)n;
    tb_update_preferred_col(tb);
    tb_publish_edit(tb, at, 0u, (size_t)n);
    return true;
}

bool sol_text_buffer_insert_newline(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    const uint8_t nl = '\n';
    const size_t at = tb->cursor_byte;
    if (!sol_rope_insert(tb->rope, at, &nl, 1u)) return false;
    tb->cursor_byte += 1u;
    tb->preferred_col_cp = 0u;
    tb_publish_edit(tb, at, 0u, 1u);
    return true;
}

bool sol_text_buffer_backspace(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    if (tb->cursor_byte == 0u) return false;
    const size_t step = tb_cp_len_before(tb->rope, tb->cursor_byte);
    if (step == 0u) return false;
    const size_t at = tb->cursor_byte - step;
    if (!sol_rope_remove(tb->rope, at, step)) return false;
    tb->cursor_byte = at;
    tb_update_preferred_col(tb);
    tb_publish_edit(tb, at, step, 0u);
    return true;
}

bool sol_text_buffer_delete_forward(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return false;
    const size_t step = tb_cp_len_at(tb->rope, tb->cursor_byte);
    if (step == 0u) return false;
    const size_t at = tb->cursor_byte;
    if (!sol_rope_remove(tb->rope, at, step)) return false;
    /* Cursor unchanged in absolute byte offset terms. */
    tb_update_preferred_col(tb);
    tb_publish_edit(tb, at, step, 0u);
    return true;
}

void sol_text_buffer_move_cursor(SolTextBuffer *tb, int dx, int dy,
                                 bool sticky_col)
{
    if (!tb || !tb->rope) return;
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

void sol_text_buffer_move_line_start(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return;
    const size_t line = sol_text_buffer_cursor_line(tb);
    tb->cursor_byte = sol_rope_byte_of_line(tb->rope, line);
    tb->preferred_col_cp = 0u;
}

void sol_text_buffer_move_line_end(SolTextBuffer *tb)
{
    if (!tb || !tb->rope) return;
    const size_t line = sol_text_buffer_cursor_line(tb);
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t line_bytes = tb_line_byte_len(tb->rope, line);
    tb->cursor_byte = line_start + line_bytes;
    tb_update_preferred_col(tb);
}

void sol_text_buffer_set_cursor_to(SolTextBuffer *tb, size_t line, size_t cp_col)
{
    if (!tb || !tb->rope) return;
    const size_t total_lines = sol_text_buffer_line_count(tb);
    if (line >= total_lines) line = total_lines ? total_lines - 1u : 0u;
    const size_t line_start = sol_rope_byte_of_line(tb->rope, line);
    const size_t col_bytes  = tb_line_byte_of_cp(tb->rope, line, cp_col);
    tb_set_cursor_byte(tb, line_start + col_bytes);
    tb->preferred_col_cp = cp_col;
}
