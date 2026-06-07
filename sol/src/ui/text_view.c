// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* text_view.c — Rope-aware text-buffer renderer + pointer handling.
 *
 * Layout per visible buffer pane:
 *
 *   buffer-scroll-row
 *     ├── buffer-gutter-col          (line numbers)
 *     ├── buffer-text-col            (text rows + caret overlay)
 *     │     owns the on_drag_start hook for click-to-position-cursor
 *     └── buffer-scrollbar           (only when content overflows)
 *
 * Each visible line is read from the rope into a small per-frame ring
 * buffer; causality keeps the pointer through the frame, so the slot
 * must outlive the build.
 */

#include "sol_text_view.h"

#include <causality.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "sol_platform.h"
#include "sol_rope.h"
#include "sol_syntax_highlight.h"
#include "sol_text_buffer.h"
#include "sol_ui_constants.h"
#include "sol_ui_system.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* Must match `.buffer-line` height in style.h. */
#define SOL_TEXT_LINE_HEIGHT_PX 20

/* Approximate UI chrome above/below the buffer pane in CSS pixels.
   Title bar (~30) + status bar (22) + tabs row (28) + buffer-text-col
   vertical padding (16) + small fudge. Only used to estimate the
   visible line count for the scrollbar thumb — over-estimating is
   safe (we just over-render and clip). */
#define SOL_TEXT_PANE_CHROME_PX 100

/* Internal vertical padding inside .buffer-text-col. */
#define SOL_TEXT_TEXT_PADDING_PX 16
#define SOL_TEXT_TEXT_PADDING_X_PX 16
#define SOL_TEXT_GUTTER_WIDTH_PX 56
#define SOL_TEXT_SCROLLBAR_WIDTH_PX 14
#define SOL_TEXT_HSCROLLBAR_HEIGHT_PX 14

/* Maximum bytes we'll read for a single visible line. Lines longer
   than this are truncated for display; the buffer content is
   untouched. 4 KiB is more than enough for typical source code while
   keeping the per-frame allocation tiny. */
#define SOL_TEXT_VIEW_MAX_LINE_BYTES 4096u

/* ------------------------------------------------------------------ */
/* Per-frame storage                                                   */
/* ------------------------------------------------------------------ */

/* Causality copies the `text` pointer descriptor but keeps it through
   the frame — we need each emitted text node to point at distinct,
   stable memory. A ring of small fixed-size slots covers the typical
   working set (worktree of ~50 visible rows per pane × a handful of
   panes). */
#define SOL_TEXT_VIEW_LINE_RING 256
static char g_line_ring[SOL_TEXT_VIEW_LINE_RING][SOL_TEXT_VIEW_MAX_LINE_BYTES];
static int  g_line_ring_cursor = 0;

/* Same idea for the line-number labels in the gutter. */
#define SOL_TEXT_VIEW_NUM_RING 256
static char g_num_ring[SOL_TEXT_VIEW_NUM_RING][16];
static int  g_num_ring_cursor = 0;

/* Click-context ring. Each rebuild emits one ctx for the text column
   (drag handler). 64 slots cover any realistic split layout. */
typedef struct TextClickCtx {
    SolUISystem        *ui;
    SolBufferNodeId     leaf_id;
    SolTextBuffer      *tb;
} TextClickCtx;

typedef struct ScrollbarDragCtx {
    SolUISystem    *ui;
    SolTextBuffer  *tb;
    bool            is_vertical;
    int             max_scroll;
    float           track_len;
    float           thumb_len;
    float           grab_offset;
} ScrollbarDragCtx;

/* Count displayed monospace columns in the first `byte_len` bytes of `buf`.
   Causality renders tabs as four invisible space advances, so editor
   selection/caret geometry must use the same visual width. */
static size_t tv_visual_col_count(const char *buf, size_t byte_len)
{
    size_t col = 0u, off = 0u;
    while (off < byte_len) {
        const uint8_t b = (uint8_t)buf[off];
        size_t step;
        if      ((b & 0x80u) == 0x00u) step = 1u;
        else if ((b & 0xE0u) == 0xC0u) step = 2u;
        else if ((b & 0xF0u) == 0xE0u) step = 3u;
        else if ((b & 0xF8u) == 0xF0u) step = 4u;
        else                            step = 1u;
        if (off + step > byte_len) break;
        if (b == '\t')
            col += 4u;
        else if ((b >= 32u && b != 0x7Fu) || b >= 0x80u)
            col += 1u;
        off += step;
    }
    return col;
}

/* Convert a rounded visual monospace column back to the buffer's codepoint
   column. Tabs are rendered as one wide glyph, so clicks inside a tab choose
   the nearest editable boundary: before it in the first half, after it in
   the second half. */
static size_t tv_cp_col_from_visual_col(const char *buf, size_t byte_len,
                                        size_t target_visual_col)
{
    size_t visual_col = 0u, cp_col = 0u, off = 0u;
    while (off < byte_len) {
        const uint8_t b = (uint8_t)buf[off];
        size_t step;
        if      ((b & 0x80u) == 0x00u) step = 1u;
        else if ((b & 0xE0u) == 0xC0u) step = 2u;
        else if ((b & 0xF0u) == 0xE0u) step = 3u;
        else if ((b & 0xF8u) == 0xF0u) step = 4u;
        else                            step = 1u;
        if (off + step > byte_len) break;

        size_t width = 0u;
        if (b == '\t')
            width = 4u;
        else if ((b >= 32u && b != 0x7Fu) || b >= 0x80u)
            width = 1u;

        if (width > 0u) {
            const size_t midpoint = visual_col + width / 2u;
            if (target_visual_col <= midpoint)
                return cp_col;
            if (target_visual_col <= visual_col + width)
                return cp_col + 1u;
            visual_col += width;
        }
        off += step;
        cp_col += 1u;
    }
    return cp_col;
}


#define SOL_TEXT_VIEW_CLICK_RING 64
static TextClickCtx g_click_ring[SOL_TEXT_VIEW_CLICK_RING];
static int          g_click_ring_cursor = 0;

#define SOL_TEXT_VIEW_SCROLLBAR_RING 64
static ScrollbarDragCtx g_scrollbar_ring[SOL_TEXT_VIEW_SCROLLBAR_RING];
static int              g_scrollbar_ring_cursor = 0;

static SolTextBuffer *g_scrollbar_drag_tb = NULL;
static bool           g_scrollbar_drag_vertical = false;
static float          g_scrollbar_drag_grab_offset = 0.0f;
static bool           g_scrollbar_drag_active = false;

static char *acquire_line_slot(void)
{
    return g_line_ring[g_line_ring_cursor++ & (SOL_TEXT_VIEW_LINE_RING - 1)];
}
static char *acquire_num_slot(void)
{
    return g_num_ring[g_num_ring_cursor++ & (SOL_TEXT_VIEW_NUM_RING - 1)];
}
static TextClickCtx *acquire_click_slot(void)
{
    return &g_click_ring[g_click_ring_cursor++ & (SOL_TEXT_VIEW_CLICK_RING - 1)];
}
static ScrollbarDragCtx *acquire_scrollbar_slot(void)
{
    return &g_scrollbar_ring[
        g_scrollbar_ring_cursor++ & (SOL_TEXT_VIEW_SCROLLBAR_RING - 1)];
}

/* Per-frame token-segment storage.
 * Each highlighted line can emit multiple ca_text nodes (one per colored
 * token / plain gap).  They need stable memory through the frame, so we
 * use a secondary ring analogous to the line ring above. */
#define SOL_TEXT_VIEW_TOKEN_RING 2048u
#define SOL_TEXT_VIEW_TOKEN_MAX   512u
static char g_token_ring[SOL_TEXT_VIEW_TOKEN_RING][SOL_TEXT_VIEW_TOKEN_MAX];
static int  g_token_ring_cursor = 0;

static char *acquire_token_slot(void)
{
    return g_token_ring[g_token_ring_cursor++ & (SOL_TEXT_VIEW_TOKEN_RING - 1u)];
}

/* ------------------------------------------------------------------ */
/* Caret blink                                                         */
/* ------------------------------------------------------------------ */

/* Standard editor blink: 530 ms on / 530 ms off (~1 Hz).             */
#define SOL_CARET_BLINK_HALF_MS  530u
/* Show caret solid for this long after the cursor moves.             */
#define SOL_CARET_SOLID_MS       150u

static uint64_t g_caret_last_move_ms = 0u;
static size_t   g_caret_prev_line    = (size_t)-1;
static size_t   g_caret_prev_col     = (size_t)-1;

static uint64_t monotonic_ms(void)
{
    return sol_platform_now_monotonic_ns() / 1000000ull;
}

/* Call once per frame with the current cursor position.  Returns true
 * when the caret should be drawn (on-phase or just moved). */
static bool caret_blink_visible(size_t cur_line, size_t cur_col)
{
    const uint64_t now = monotonic_ms();
    if (cur_line != g_caret_prev_line || cur_col != g_caret_prev_col) {
        g_caret_prev_line    = cur_line;
        g_caret_prev_col     = cur_col;
        g_caret_last_move_ms = now;
    }
    /* Always solid immediately after movement. */
    if (now - g_caret_last_move_ms < SOL_CARET_SOLID_MS) return true;
    /* Periodic blink phase relative to last move. */
    const uint64_t phase = (now - g_caret_last_move_ms)
                           % (SOL_CARET_BLINK_HALF_MS * 2u);
    return phase < SOL_CARET_BLINK_HALF_MS;
}

/* ------------------------------------------------------------------ */
/* Syntax-highlight helpers                                            */
/* ------------------------------------------------------------------ */

/* Emit ca_text nodes for a single line using the pre-queried span list.
 * Spans must be sorted by start_byte and already clipped/filtered to
 * the range [line_start_byte, line_start_byte + line_bytes). */
static void emit_highlighted_line(
    const char          *line_buf,
    size_t               line_bytes,
    uint32_t             line_start_byte,
    const SolSyntaxSpan *spans,
    size_t               span_count)
{
    if (line_bytes == 0u) {
        /* Empty line — emit a space so the row keeps its layout height. */
        ca_text(&(Ca_TextDesc){ .text = " ", .style = "hl-plain" });
        return;
    }
    if (span_count == 0u) {
        ca_text(&(Ca_TextDesc){
            .text  = line_buf,
            .style = "hl-plain",
        });
        return;
    }

    const uint32_t line_end_byte = line_start_byte + (uint32_t)line_bytes;
    uint32_t pos = line_start_byte;   /* cursor in document bytes */

    for (size_t i = 0u; i < span_count; i++) {
        uint32_t sp_start = spans[i].start_byte;
        uint32_t sp_end   = spans[i].end_byte;
        /* Clamp span to line boundaries */
        if (sp_start < line_start_byte) sp_start = line_start_byte;
        if (sp_end   > line_end_byte)   sp_end   = line_end_byte;
        if (sp_start >= sp_end || sp_start < pos) continue;

        /* Plain gap before this colored span */
        if (pos < sp_start) {
            size_t off = pos - line_start_byte;
            size_t len = sp_start - pos;
            if (len >= SOL_TEXT_VIEW_TOKEN_MAX) len = SOL_TEXT_VIEW_TOKEN_MAX - 1u;
            char *slot = acquire_token_slot();
            memcpy(slot, line_buf + off, len);
            slot[len] = '\0';
            ca_text(&(Ca_TextDesc){ .text = slot, .style = "hl-plain" });
        }

        /* Colored token */
        {
            size_t off = sp_start - line_start_byte;
            size_t len = sp_end - sp_start;
            if (len >= SOL_TEXT_VIEW_TOKEN_MAX) len = SOL_TEXT_VIEW_TOKEN_MAX - 1u;
            char *slot = acquire_token_slot();
            memcpy(slot, line_buf + off, len);
            slot[len] = '\0';
            ca_text(&(Ca_TextDesc){ .text = slot, .style = spans[i].css_class });
        }

        pos = sp_end;
    }

    /* Trailing plain text after the last span */
    if (pos < line_end_byte) {
        size_t off = pos - line_start_byte;
        size_t len = line_end_byte - pos;
        if (len >= SOL_TEXT_VIEW_TOKEN_MAX) len = SOL_TEXT_VIEW_TOKEN_MAX - 1u;
        char *slot = acquire_token_slot();
        memcpy(slot, line_buf + off, len);
        slot[len] = '\0';
        ca_text(&(Ca_TextDesc){ .text = slot, .style = "hl-plain" });
    }
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

int sol_text_view_visible_lines_for_height(float pane_h, float ui_scale);

int sol_text_view_visible_lines(int window_h, float ui_scale)
{
    if (ui_scale <= 0.0f) ui_scale = 1.0f;
    return sol_text_view_visible_lines_for_height(
        (float)window_h - (float)SOL_TEXT_PANE_CHROME_PX * ui_scale,
        ui_scale);
}

int sol_text_view_visible_lines_for_height(float pane_h, float ui_scale)
{
    if (ui_scale <= 0.0f) ui_scale = 1.0f;
    /* Scale the CSS constants to layout pixels (= GLFW logical px). */
    int line_h = (int)(SOL_TEXT_LINE_HEIGHT_PX * ui_scale + 0.5f);
    if (line_h < 1) line_h = 1;
    int chrome = (int)(SOL_TEXT_TEXT_PADDING_PX * ui_scale + 0.5f);
    int avail = (int)(pane_h + 0.5f) - chrome;
    if (avail < line_h) avail = line_h;
    int n = avail / line_h;
    /* Over-render by two so the pane always looks fully filled even
       when the chrome estimate is off. The parent has overflow:hidden,
       so extra rows just clip. */
    n += 2;
    if (n < 1) n = 1;
    return n;
}

int sol_text_view_visible_cols_for_width(float pane_w, float ui_scale,
                                         float glyph_advance_layout_px)
{
    if (ui_scale <= 0.0f) ui_scale = 1.0f;
    float adv = glyph_advance_layout_px / ui_scale;
    if (adv <= 0.0f) adv = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT * 0.6f;

    float avail = pane_w
        - (float)SOL_TEXT_GUTTER_WIDTH_PX
        - (float)SOL_TEXT_SCROLLBAR_WIDTH_PX
        - (float)SOL_TEXT_TEXT_PADDING_X_PX;
    if (avail < adv) avail = adv;

    int cols = (int)(avail / adv);
    if (cols < 1) cols = 1;
    return cols;
}

/* Resolve the monospace glyph advance for caret / click math. Falls
   back to a 60% ratio of the font size when the window can't measure
   yet (no font atlas warmed up). */
static float glyph_advance_px_for(Ca_Window *win)
{
    float w = win ? ca_measure_text_px(win, "M", SOL_UI_BOOT_FONT_SIZE_PX_FLOAT)
                  : 0.0f;
    if (w <= 0.0f) w = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT * 0.6f;
    return w;
}

static size_t visible_max_line_cols(const SolTextBuffer *tb, int scroll_top,
                                    int rendered)
{
    if (!tb || rendered <= 0) return 0u;
    const int total = (int)sol_text_buffer_line_count(tb);
    size_t max_cols = 0u;
    for (int i = 0; i < rendered; ++i) {
        const int line_idx = scroll_top + i;
        if (line_idx < 0 || line_idx >= total) continue;
        char line_buf[SOL_TEXT_VIEW_MAX_LINE_BYTES];
        const size_t line_bytes = sol_text_buffer_copy_line(
            tb, (size_t)line_idx, line_buf, sizeof(line_buf));
        const size_t cols = tv_visual_col_count(line_buf, line_bytes);
        if (cols > max_cols) max_cols = cols;
    }
    return max_cols;
}

/* ------------------------------------------------------------------ */
/* Pointer handler                                                     */
/* ------------------------------------------------------------------ */

static bool tv_local_to_line_col(SolUISystem *ui, SolTextBuffer *tb,
                                 float event_local_x, float event_local_y,
                                 size_t *out_line, size_t *out_cp_col)
{
    if (!ui || !tb || !out_line || !out_cp_col) {
        return false;
    }

    Ca_Window *win = sol_ui_system_primary_window(ui);
    const float scale = ca_window_get_scale(win);
    const float pad_x = 8.0f * scale;
    const float pad_y = 8.0f * scale;
    float local_x = event_local_x - pad_x;
    float local_y = event_local_y - pad_y;
    if (local_x < 0.0f) local_x = 0.0f;
    if (local_y < 0.0f) local_y = 0.0f;

    const float line_h = (float)SOL_TEXT_LINE_HEIGHT_PX * scale;
    const int   row    = (int)(local_y / line_h);
    const int   scroll = sol_text_buffer_scroll_top(tb);
    int line_idx = scroll + row;
    if (line_idx < 0) line_idx = 0;
    const int total = (int)sol_text_buffer_line_count(tb);
    if (total <= 0) {
        *out_line = 0u;
        *out_cp_col = 0u;
        return true;
    }
    if (line_idx >= total) line_idx = total - 1;

    const float adv = glyph_advance_px_for(win);
    int visual_col = sol_text_buffer_scroll_left(tb)
        + (int)((local_x / adv) + 0.5f);
    if (visual_col < 0) visual_col = 0;

    char line_buf[SOL_TEXT_VIEW_MAX_LINE_BYTES];
    const size_t line_bytes = sol_text_buffer_copy_line(
        tb, (size_t)line_idx, line_buf, sizeof(line_buf));
    const size_t cp_col = tv_cp_col_from_visual_col(
        line_buf, line_bytes, (size_t)visual_col);

    *out_line = (size_t)line_idx;
    *out_cp_col = cp_col;
    return true;
}

bool sol_text_view_local_point_to_line_col(SolUISystem *ui,
                                           SolTextBuffer *tb,
                                           float local_x,
                                           float local_y,
                                           size_t *out_line,
                                           size_t *out_cp_col)
{
    return tv_local_to_line_col(ui, tb, local_x, local_y,
                                out_line, out_cp_col);
}

/* Convert pane-local drag event coordinates to line/codepoint column. */
static void tv_ev_to_line_col(const Ca_DragEvent *ev, const TextClickCtx *cb,
                               int *out_line, int *out_cp_col)
{
    size_t line = 0u;
    size_t cp_col = 0u;
    if (!tv_local_to_line_col(cb->ui, cb->tb, ev->local_x, ev->local_y,
                              &line, &cp_col)) {
        *out_line = 0;
        *out_cp_col = 0;
        return;
    }
    *out_line = (int)line;
    *out_cp_col = (int)cp_col;
}

/* on_drag_start — click or start of drag.  Set cursor and store anchor
   for potential selection extension via on_drag. */
static void on_text_col_drag_start(const Ca_DragEvent *ev, void *user_data)
{
    TextClickCtx *cb = (TextClickCtx *)user_data;
    if (!ev || !cb || !cb->ui || !cb->tb) return;

    sol_ui_system_focus_leaf(cb->ui, cb->leaf_id);

    int line_idx, cp_col;
    tv_ev_to_line_col(ev, cb, &line_idx, &cp_col);

    /* Clear any existing selection and position cursor. */
    sol_text_buffer_set_cursor_to(cb->tb, (size_t)line_idx, (size_t)cp_col);
    /* Store anchor for drag — selection becomes active only when the
       cursor moves away during on_drag. */
    sol_text_buffer_set_selection_anchor(cb->tb);
    sol_ui_system_invalidate_buffer_area(cb->ui);
}

/* on_drag — mouse moved while button held.  Extend selection. */
static void on_text_col_drag_move(const Ca_DragEvent *ev, void *user_data)
{
    TextClickCtx *cb = (TextClickCtx *)user_data;
    if (!ev || !cb || !cb->ui || !cb->tb) return;

    int line_idx, cp_col;
    tv_ev_to_line_col(ev, cb, &line_idx, &cp_col);

    sol_text_buffer_set_cursor_to_sel(cb->tb, (size_t)line_idx,
                                      (size_t)cp_col, /*extend=*/true);
    sol_ui_system_invalidate_buffer_area(cb->ui);
}

static void on_scrollbar_drag_start(const Ca_DragEvent *ev, void *user_data)
{
    ScrollbarDragCtx *ctx = (ScrollbarDragCtx *)user_data;
    if (!ev || !ctx || !ctx->tb) return;

    const int current = ctx->is_vertical
        ? sol_text_buffer_scroll_top(ctx->tb)
        : sol_text_buffer_scroll_left(ctx->tb);
    const float travel = ctx->track_len - ctx->thumb_len;
    const float pct = (ctx->max_scroll > 0 && travel > 0.0f)
        ? (float)current / (float)ctx->max_scroll : 0.0f;
    const float thumb_pos = pct * travel;
    const float pointer = ctx->is_vertical ? ev->local_y : ev->local_x;

    if (pointer >= thumb_pos && pointer <= thumb_pos + ctx->thumb_len) {
        ctx->grab_offset = pointer - thumb_pos;
    } else {
        ctx->grab_offset = ctx->thumb_len * 0.5f;
    }
    g_scrollbar_drag_tb = ctx->tb;
    g_scrollbar_drag_vertical = ctx->is_vertical;
    g_scrollbar_drag_grab_offset = ctx->grab_offset;
    g_scrollbar_drag_active = true;
}

static void on_scrollbar_drag(const Ca_DragEvent *ev, void *user_data)
{
    ScrollbarDragCtx *ctx = (ScrollbarDragCtx *)user_data;
    if (!ev || !ctx || !ctx->tb || ctx->max_scroll <= 0) return;

    const float travel = ctx->track_len - ctx->thumb_len;
    if (travel <= 0.0f) return;

    const float pointer = ctx->is_vertical ? ev->local_y : ev->local_x;
    float grab_offset = ctx->grab_offset;
    if (g_scrollbar_drag_active &&
        g_scrollbar_drag_tb == ctx->tb &&
        g_scrollbar_drag_vertical == ctx->is_vertical) {
        grab_offset = g_scrollbar_drag_grab_offset;
    }
    float pct = (pointer - grab_offset) / travel;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    int scroll = (int)(pct * (float)ctx->max_scroll + 0.5f);
    if (scroll < 0) scroll = 0;
    if (scroll > ctx->max_scroll) scroll = ctx->max_scroll;

    if (ctx->is_vertical) {
        sol_text_buffer_set_scroll_top(ctx->tb, scroll);
    } else {
        sol_text_buffer_set_scroll_left(ctx->tb, scroll);
    }
    if (ctx->ui) sol_ui_system_invalidate_buffer_area(ctx->ui);
}

static void on_scrollbar_drag_end(const Ca_DragEvent *ev, void *user_data)
{
    (void)ev;
    (void)user_data;
    g_scrollbar_drag_tb = NULL;
    g_scrollbar_drag_vertical = false;
    g_scrollbar_drag_grab_offset = 0.0f;
    g_scrollbar_drag_active = false;
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

void sol_text_view_render(const SolBuffer *buffer,
                          const SolBufferRenderArgs *args,
                          void *state)
{
    (void)buffer;
    SolTextBuffer *tb = (SolTextBuffer *)state;
    if (!tb) return;

    SolUISystem *ui = args ? (SolUISystem *)args->ui_context : NULL;

    /* Get current scale so viewport count and geometry are correct. */
    Ca_Window *primary_win = ui ? sol_ui_system_primary_window(ui) : NULL;
    const float ui_scale = primary_win ? ca_window_get_scale(primary_win) : 1.0f;
    float pane_h = args ? args->rect.h : 0.0f;
    if (pane_h <= 0.0f) {
        int win_h = 0;
        if (ui) sol_ui_system_window_size(ui, NULL, &win_h);
        if (win_h <= 0) win_h = 600;
        pane_h = (float)win_h - (float)SOL_TEXT_PANE_CHROME_PX * ui_scale;
    }

    /* `rendered` is what we emit (over-rendered to fill the pane);
       `viewport` is what the user actually sees and drives the
       scrollbar thumb math. */
    const int rendered = sol_text_view_visible_lines_for_height(pane_h, ui_scale);
    int viewport = rendered - 2;
    if (viewport < 1) viewport = 1;

    const int total   = (int)sol_text_buffer_line_count(tb);
    const int max_top = total > viewport ? total - viewport : 0;
    int scroll_top = sol_text_buffer_scroll_top(tb);
    if (scroll_top > max_top) scroll_top = max_top;
    if (scroll_top < 0)       scroll_top = 0;
    /* Clamp back into the state in case scroll drifted. */
    if (scroll_top != sol_text_buffer_scroll_top(tb)) {
        sol_text_buffer_set_scroll_top(tb, scroll_top);
    }

    const size_t cur_line = sol_text_buffer_cursor_line(tb);
    const size_t cur_col  = sol_text_buffer_cursor_col(tb);
    const float adv_css = glyph_advance_px_for(primary_win) / ui_scale;
    const int viewport_cols = sol_text_view_visible_cols_for_width(
        args ? args->rect.w : 0.0f, ui_scale, adv_css * ui_scale);
    const size_t max_line_cols = visible_max_line_cols(tb, scroll_top, rendered);
    const int max_left = max_line_cols > (size_t)viewport_cols
        ? (int)(max_line_cols - (size_t)viewport_cols) : 0;
    int scroll_left = sol_text_buffer_scroll_left(tb);
    if (scroll_left > max_left) scroll_left = max_left;
    if (scroll_left < 0) scroll_left = 0;
    if (scroll_left != sol_text_buffer_scroll_left(tb)) {
        sol_text_buffer_set_scroll_left(tb, scroll_left);
    }
    const float scroll_x = (float)scroll_left * adv_css;
    float line_content_w = (float)(max_line_cols + 1u) * adv_css;
    if (line_content_w < (float)(viewport_cols + 1) * adv_css) {
        line_content_w = (float)(viewport_cols + 1) * adv_css;
    }
    float pane_w = args ? args->rect.w : 0.0f;
    if (pane_w <= 0.0f) {
        int win_w = 0;
        if (ui) sol_ui_system_window_size(ui, &win_w, NULL);
        if (win_w <= 0) win_w = 800;
        pane_w = (float)win_w;
    }
    float text_track_w = pane_w
        - (float)SOL_TEXT_GUTTER_WIDTH_PX
        - (float)SOL_TEXT_SCROLLBAR_WIDTH_PX;
    if (text_track_w < adv_css) text_track_w = (float)viewport_cols * adv_css;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "buffer-scroll-row",
    });

    /* -------- Gutter (line numbers) -------- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "buffer-gutter-col",
    });
    for (int i = 0; i < rendered; ++i) {
        const int line_idx = scroll_top + i;
        /* Always emit a div wrapper so the node type stays consistent
           across frames (avoids stale-paint artifacts when positions
           transition between valid and past-end on scroll). */
        ca_div_begin(&(Ca_DivDesc){ .style = "buffer-gutter-line-empty" });
        if (line_idx < total) {
            char *slot = acquire_num_slot();
            snprintf(slot, 16, "%d", line_idx + 1);
            ca_text(&(Ca_TextDesc){
                .text  = slot,
                .style = "buffer-gutter-line",
            });
        }
        ca_div_end();
    }
    ca_div_end();   /* buffer-gutter-col */

    /* -------- Text column -------- */
    TextClickCtx *cb = acquire_click_slot();
    cb->ui      = ui;
    cb->leaf_id = args ? args->leaf_id : 0u;
    cb->tb      = tb;

    ca_div_begin(&(Ca_DivDesc){
        .direction     = CA_VERTICAL,
        .style         = "buffer-text-col",
        .on_drag_start = ui ? on_text_col_drag_start : NULL,
        .on_drag       = ui ? on_text_col_drag_move  : NULL,
        .drag_data     = cb,
    });

    /* Pre-compute selection range (byte offsets) once per frame.
       Shown on any pane that holds a selection, regardless of focus. */
    const bool sel_active = sol_text_buffer_has_selection(tb);
    size_t sel_start = 0u, sel_end = 0u;
    if (sel_active)
        sol_text_buffer_selection_range(tb, &sel_start, &sel_end);

    /* CSS-px glyph advance used for selection geometry (pre-scaled). */
    const float sel_adv = adv_css;
    /* Rope reference for per-line byte offset queries. */
    const SolRope *rope_ref = sol_text_buffer_rope((SolBuffer *)buffer);
    /* Selection highlight colour — dark steel blue. */
    const uint32_t SEL_COLOR = ca_color(0.14f, 0.21f, 0.37f, 1.0f);

    for (int i = 0; i < rendered; ++i) {
        const int line_idx = scroll_top + i;
        if (line_idx >= total) {
            ca_div_begin(&(Ca_DivDesc){ .style = "buffer-line-empty" });
            ca_div_end();
            continue;
        }

        /* Read the line into a per-frame slot. */
        char *line_buf = acquire_line_slot();
        const size_t line_bytes = sol_text_buffer_copy_line(
            tb, (size_t)line_idx, line_buf, SOL_TEXT_VIEW_MAX_LINE_BYTES);

        const bool is_cursor_line =
            args && args->is_active && (size_t)line_idx == cur_line;

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "buffer-line-row",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .position  = CA_POSITION_ABSOLUTE,
            .pos_x     = -scroll_x,
            .pos_y     = 0.0f,
            .width     = line_content_w,
            .height    = (float)SOL_TEXT_LINE_HEIGHT_PX,
            .style     = "buffer-line-content",
        });

        /* ---- Selection highlight (behind text, z_index = -1) ---- */
        if (sel_active && rope_ref) {
            const size_t lb_start = sol_rope_byte_of_line(rope_ref, (size_t)line_idx);
            const size_t lb_end   = lb_start + line_bytes;
            /* +1 to include the newline so selection extends past EOL. */
            if (sel_start < lb_end + 1u && sel_end > lb_start) {
                /* Byte offsets of sel within this line's content. */
                size_t col_b_start = sel_start > lb_start
                    ? sel_start - lb_start : 0u;
                size_t col_b_end   = sel_end   < lb_end
                    ? sel_end   - lb_start : line_bytes;
                if (col_b_end > line_bytes) col_b_end = line_bytes;
                /* Convert byte offsets → rendered monospace columns. */
                const float cp_s = (float)tv_visual_col_count(line_buf, col_b_start);
                const float cp_e = (float)tv_visual_col_count(line_buf, col_b_end);
                float x1 = cp_s * sel_adv;
                float x2 = cp_e * sel_adv;
                /* If selection extends past this line, add a bit of
                   extra highlight for the newline. */
                if (sel_end > lb_end && x2 < x1 + sel_adv * 0.5f)
                    x2 = x1 + sel_adv * 0.5f;
                if (x2 <= x1) x2 = x1 + 2.0f;
                ca_div_begin(&(Ca_DivDesc){
                    .position   = CA_POSITION_ABSOLUTE,
                    .pos_x      = x1,
                    .pos_y      = 0.0f,
                    .width      = x2 - x1,
                    .height     = (float)SOL_TEXT_LINE_HEIGHT_PX,
                    .background = SEL_COLOR,
                });
                ca_div_end();
            }
        }

        /* Emit line content — tokenized when a syntax highlighter is
         * available, plain otherwise. */
        SolSyntaxHighlighter *hl = sol_text_buffer_highlighter(tb);
        if (hl && sol_syntax_highlight_is_valid(hl)) {
            const SolRope *rope =
                sol_text_buffer_rope((SolBuffer *)buffer);
            uint32_t line_start = rope
                ? (uint32_t)sol_rope_byte_of_line(rope, (size_t)line_idx)
                : 0u;
            SolSyntaxSpan spans[64];
            size_t span_count = sol_syntax_highlight_spans_for_range(
                hl, line_start,
                line_start + (uint32_t)line_bytes,
                spans, 64u);
            emit_highlighted_line(
                line_buf, line_bytes, line_start, spans, span_count);
        } else {
            ca_text(&(Ca_TextDesc){
                .text  = line_bytes > 0u ? line_buf : " ",
                .style = "buffer-line",
            });
        }

        if (is_cursor_line) {
            /* Count rendered columns in the byte prefix [0, cur_col). */
            const size_t cp_count = tv_visual_col_count(
                line_buf, cur_col < line_bytes ? cur_col : line_bytes);
            Ca_Window *const caret_win = sol_ui_system_primary_window(ui);

            /* ca_measure_text_px and ca_font_line_metrics both return
             * values in LAYOUT SPACE (CSS px × ui_scale).  Ca_DivDesc
             * positional/size fields expect CSS pixels — div_to_nd
             * applies the scale factor internally via s().  Passing
             * layout-space values here would double-scale them at
             * ui_scale != 1.0, causing the caret to drift right and
             * grow too tall.  Divide by ui_scale to convert back to
             * CSS px before handing off to Ca_DivDesc. */
            const float adv = glyph_advance_px_for(caret_win) / ui_scale;
            const float caret_x = (float)cp_count * adv;
            const bool  visible = caret_blink_visible(cur_line, cur_col);

            /* Fallbacks in layout space (matching what ca_font_line_metrics
             * would return if a font were already loaded). */
            float c_ascent = 11.0f * ui_scale, c_descent = -3.0f * ui_scale;
            ca_font_line_metrics(caret_win, 12.0f, &c_ascent, &c_descent);
            /* Convert layout-space metrics to CSS px. */
            const float c_em_h = (c_ascent - c_descent) / ui_scale;
            const float c_y    = (SOL_TEXT_LINE_HEIGHT_PX - c_em_h) * 0.5f;

            ca_div_begin(&(Ca_DivDesc){
                .position = CA_POSITION_ABSOLUTE,
                .pos_x    = caret_x,
                .pos_y    = c_y,
                .width    = 2.0f,
                .height   = c_em_h,
                .background = ca_color(1.0f, 1.0f, 1.0f, visible ? 1.0f : 0.0f),
            });
            ca_div_end();
        }

        ca_div_end();   /* buffer-line-content */
        ca_div_end();   /* buffer-line-row */
    }

    if (max_left > 0) {
        const float track_w = text_track_w;
        float thumb_w = track_w * (float)viewport_cols / (float)max_line_cols;
        if (thumb_w < 24.0f) thumb_w = 24.0f;
        if (thumb_w > track_w) thumb_w = track_w;
        const float free_w = track_w - thumb_w;
        const float left_spacer = free_w * (float)scroll_left / (float)max_left;
        float hbar_y = pane_h - (float)SOL_TEXT_HSCROLLBAR_HEIGHT_PX;
        if (hbar_y < 0.0f) hbar_y = 0.0f;

        ScrollbarDragCtx *hctx = acquire_scrollbar_slot();
        hctx->ui = ui;
        hctx->tb = tb;
        hctx->is_vertical = false;
        hctx->max_scroll = max_left;
        hctx->track_len = track_w;
        hctx->thumb_len = thumb_w;
        hctx->grab_offset = 0.0f;

        ca_div_begin(&(Ca_DivDesc){
            .direction     = CA_HORIZONTAL,
            .position      = CA_POSITION_ABSOLUTE,
            .pos_x         = 0.0f,
            .pos_y         = hbar_y,
            .width         = track_w,
            .height        = (float)SOL_TEXT_HSCROLLBAR_HEIGHT_PX,
            .style         = "buffer-hscrollbar",
            .on_drag_start = on_scrollbar_drag_start,
            .on_drag       = on_scrollbar_drag,
            .on_drag_end   = on_scrollbar_drag_end,
            .drag_data     = hctx,
        });
        if (left_spacer >= 0.5f) {
            ca_div_begin(&(Ca_DivDesc){
                .style = "buffer-hscrollbar-spacer",
                .width = left_spacer,
            });
            ca_div_end();
        }
        ca_div_begin(&(Ca_DivDesc){
            .style = args && args->is_active
                ? "buffer-hscrollbar-thumb buffer-hscrollbar-thumb-active"
                : "buffer-hscrollbar-thumb",
            .width = thumb_w,
            .height = (float)SOL_TEXT_HSCROLLBAR_HEIGHT_PX,
        });
        ca_div_end();
        ca_div_end();   /* buffer-hscrollbar */
    }
    ca_div_end();   /* buffer-text-col */
    sol_ui_system_attach_buffer_text_context_menu(
        ui, args ? args->leaf_id : 0u, buffer ? sol_buffer_id(buffer) : 0u);

    /* Blink redraws are driven by sol_ui_on_frame (workspace.c), which
     * bumps sig_buffer_rev and posts a wake event every tick while an
     * active buffer is focused.  Nothing to do here. */
    if (total > viewport && max_top > 0) {
        /* Scrollbar thumb and spacer heights are passed as Ca_DivDesc.height
         * which expects CSS pixels (div_to_nd scales internally).  Use
         * SOL_TEXT_LINE_HEIGHT_PX directly — do NOT multiply by ui_scale
         * here, otherwise at ui_scale > 1.0 the thumb would be double-scaled
         * and appear oversized / mispositioned. */
        const float track_h     = pane_h;
        float thumb_h           = track_h * (float)viewport / (float)total;
        if (thumb_h < 16.0f) thumb_h = 16.0f;
        if (thumb_h > track_h) thumb_h = track_h;
        const float free_h      = track_h - thumb_h;
        const float top_spacer  = free_h * (float)scroll_top / (float)max_top;

        ScrollbarDragCtx *vctx = acquire_scrollbar_slot();
        vctx->ui = ui;
        vctx->tb = tb;
        vctx->is_vertical = true;
        vctx->max_scroll = max_top;
        vctx->track_len = track_h;
        vctx->thumb_len = thumb_h;
        vctx->grab_offset = 0.0f;

        ca_div_begin(&(Ca_DivDesc){
            .direction     = CA_VERTICAL,
            .style         = "buffer-scrollbar",
            .on_drag_start = on_scrollbar_drag_start,
            .on_drag       = on_scrollbar_drag,
            .on_drag_end   = on_scrollbar_drag_end,
            .drag_data     = vctx,
        });
        /* Skip a zero-height spacer — causality treats .height == 0 as
           auto/flex which would push the thumb to the bottom. */
        if (top_spacer >= 0.5f) {
            ca_div_begin(&(Ca_DivDesc){
                .style  = "buffer-scrollbar-spacer",
                .height = top_spacer,
            });
            ca_div_end();
        }
        ca_div_begin(&(Ca_DivDesc){
            .style  = args && args->is_active
                          ? "buffer-scrollbar-thumb buffer-scrollbar-thumb-active"
                          : "buffer-scrollbar-thumb",
            .height = thumb_h,
        });
        ca_div_end();
        ca_div_end();   /* buffer-scrollbar */
    }

    ca_div_end();   /* buffer-scroll-row */
}
