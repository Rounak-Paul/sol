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
#include <time.h>

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

#define SOL_TEXT_VIEW_CLICK_RING 64
static TextClickCtx g_click_ring[SOL_TEXT_VIEW_CLICK_RING];
static int          g_click_ring_cursor = 0;

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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
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

int sol_text_view_visible_lines(int window_h, float ui_scale)
{
    if (ui_scale <= 0.0f) ui_scale = 1.0f;
    /* Scale the CSS constants to layout pixels (= GLFW logical px). */
    int line_h = (int)(SOL_TEXT_LINE_HEIGHT_PX * ui_scale + 0.5f);
    if (line_h < 1) line_h = 1;
    int chrome = (int)(SOL_TEXT_PANE_CHROME_PX * ui_scale + 0.5f);
    int avail = window_h - chrome;
    if (avail < line_h) avail = line_h;
    int n = avail / line_h;
    /* Over-render by two so the pane always looks fully filled even
       when the chrome estimate is off. The parent has overflow:hidden,
       so extra rows just clip. */
    n += 2;
    if (n < 1) n = 1;
    return n;
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

/* ------------------------------------------------------------------ */
/* Pointer handler                                                     */
/* ------------------------------------------------------------------ */

static void on_text_col_drag(const Ca_DragEvent *ev, void *user_data)
{
    TextClickCtx *cb = (TextClickCtx *)user_data;
    if (!ev || !cb || !cb->ui || !cb->tb) return;

    /* Focus the host pane first so subsequent typing lands here. */
    sol_ui_system_focus_leaf(cb->ui, cb->leaf_id);

    /* Convert pane-local (x, y) → (line, codepoint column). The text
       column has 8 px padding all around; scale the CSS constant to
       layout pixels so clicks map to the right row at any ui_scale. */
    Ca_Window *click_win = sol_ui_system_primary_window(cb->ui);
    const float scale = ca_window_get_scale(click_win);
    const float pad_x = 8.0f * scale;
    const float pad_y = 8.0f * scale;
    float local_x = ev->local_x - pad_x;
    float local_y = ev->local_y - pad_y;
    if (local_x < 0.0f) local_x = 0.0f;
    if (local_y < 0.0f) local_y = 0.0f;

    const float line_h_layout = SOL_TEXT_LINE_HEIGHT_PX * scale;
    const int row = (int)(local_y / line_h_layout);
    const int scroll_top = sol_text_buffer_scroll_top(cb->tb);
    int line_idx = scroll_top + row;
    if (line_idx < 0) line_idx = 0;
    const int total = (int)sol_text_buffer_line_count(cb->tb);
    if (line_idx >= total) line_idx = total - 1;

    const float adv = glyph_advance_px_for(click_win);
    int target_cp = (int)((local_x / adv) + 0.5f);
    if (target_cp < 0) target_cp = 0;

    sol_text_buffer_set_cursor_to(cb->tb, (size_t)line_idx, (size_t)target_cp);
    /* Cursor changed → buffer rev gets bumped through... nothing,
       actually — the cursor lives on SolTextBuffer, not the buffer
       system. Force a buffer-area rebuild explicitly. */
    sol_ui_system_invalidate_buffer_area(cb->ui);
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

    int win_h = 0;
    if (ui) sol_ui_system_window_size(ui, NULL, &win_h);
    if (win_h <= 0) win_h = 600;

    /* Get current scale so viewport count and geometry are correct. */
    Ca_Window *primary_win = ui ? sol_ui_system_primary_window(ui) : NULL;
    const float ui_scale = ca_window_get_scale(primary_win);

    /* `rendered` is what we emit (over-rendered to fill the pane);
       `viewport` is what the user actually sees and drives the
       scrollbar thumb math. */
    const int rendered = sol_text_view_visible_lines(win_h, ui_scale);
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
        .on_drag_start = ui ? on_text_col_drag : NULL,
        .drag_data     = cb,
    });

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
            /* Count codepoints in the byte prefix [0, cur_col). */
            size_t cp_count = 0u;
            for (size_t off = 0u; off < cur_col && off < line_bytes; ) {
                const uint8_t b = (uint8_t)line_buf[off];
                size_t step;
                if ((b & 0x80u) == 0x00u) step = 1u;
                else if ((b & 0xE0u) == 0xC0u) step = 2u;
                else if ((b & 0xF0u) == 0xE0u) step = 3u;
                else if ((b & 0xF8u) == 0xF0u) step = 4u;
                else step = 1u;
                if (off + step > cur_col || off + step > line_bytes) break;
                off += step;
                ++cp_count;
            }
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

        ca_div_end();   /* buffer-line-row */
    }
    ca_div_end();   /* buffer-text-col */

    /* Blink redraws are driven by sol_ui_on_frame (workspace.c), which
     * bumps sig_buffer_rev and posts a wake event every tick while an
     * active buffer is focused.  Nothing to do here. */
    if (total > viewport && max_top > 0) {
        /* Scrollbar thumb and spacer heights are passed as Ca_DivDesc.height
         * which expects CSS pixels (div_to_nd scales internally).  Use
         * SOL_TEXT_LINE_HEIGHT_PX directly — do NOT multiply by ui_scale
         * here, otherwise at ui_scale > 1.0 the thumb would be double-scaled
         * and appear oversized / mispositioned. */
        const float track_h     = (float)viewport * (float)SOL_TEXT_LINE_HEIGHT_PX;
        float thumb_h           = track_h * (float)viewport / (float)total;
        if (thumb_h < 16.0f) thumb_h = 16.0f;
        if (thumb_h > track_h) thumb_h = track_h;
        const float free_h      = track_h - thumb_h;
        const float top_spacer  = free_h * (float)scroll_top / (float)max_top;

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "buffer-scrollbar",
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
