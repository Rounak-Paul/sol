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

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

int sol_text_view_visible_lines(int window_h)
{
    int avail = window_h - SOL_TEXT_PANE_CHROME_PX;
    if (avail < SOL_TEXT_LINE_HEIGHT_PX) avail = SOL_TEXT_LINE_HEIGHT_PX;
    int n = avail / SOL_TEXT_LINE_HEIGHT_PX;
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
       column has 8 px padding all around. */
    const float pad_x = 8.0f;
    const float pad_y = 8.0f;
    float local_x = ev->local_x - pad_x;
    float local_y = ev->local_y - pad_y;
    if (local_x < 0.0f) local_x = 0.0f;
    if (local_y < 0.0f) local_y = 0.0f;

    const int row = (int)(local_y / (float)SOL_TEXT_LINE_HEIGHT_PX);
    const int scroll_top = sol_text_buffer_scroll_top(cb->tb);
    int line_idx = scroll_top + row;
    if (line_idx < 0) line_idx = 0;
    const int total = (int)sol_text_buffer_line_count(cb->tb);
    if (line_idx >= total) line_idx = total - 1;

    const float adv = glyph_advance_px_for(sol_ui_system_primary_window(cb->ui));
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

    /* `rendered` is what we emit (over-rendered to fill the pane);
       `viewport` is what the user actually sees and drives the
       scrollbar thumb math. */
    const int rendered = sol_text_view_visible_lines(win_h);
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

        ca_text(&(Ca_TextDesc){
            .text  = line_bytes > 0u ? line_buf : " ",
            .style = "buffer-line",
        });

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
            const float adv = glyph_advance_px_for(
                ui ? sol_ui_system_primary_window(ui) : NULL);
            const float caret_x = (float)cp_count * adv;
            ca_div_begin(&(Ca_DivDesc){
                .style    = "buffer-caret",
                .position = CA_POSITION_ABSOLUTE,
                .pos_x    = caret_x,
                .pos_y    = 1.0f,
            });
            ca_div_end();
        }

        ca_div_end();   /* buffer-line-row */
    }
    ca_div_end();   /* buffer-text-col */

    /* -------- Scrollbar -------- */
    if (total > viewport && max_top > 0) {
        const float track_h     = (float)(viewport * SOL_TEXT_LINE_HEIGHT_PX);
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
