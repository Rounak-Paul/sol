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
#include <stdlib.h>
#include <string.h>

#include "sol_platform.h"
#include "sol_markdown.h"
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
   Title bar (~30) + status bar (22) + tabs row (19) + buffer-text-col
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

/* Built-in rulers shared by every text buffer. */
static const size_t SOL_TEXT_RULER_COLUMNS[] = { 80u, 100u };

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

/* Inline Markdown spans borrow stable storage through the current frame. */
#define SOL_MARKDOWN_PREVIEW_LINE_RING 4096u
static char g_markdown_preview_ring[SOL_MARKDOWN_PREVIEW_LINE_RING]
                                   [SOL_TEXT_VIEW_MAX_LINE_BYTES];
static size_t g_markdown_preview_ring_cursor = 0u;

/* Return a stable string slot for a rendered Markdown block. */
static char *acquire_markdown_preview_slot(void)
{
    return g_markdown_preview_ring[
        g_markdown_preview_ring_cursor++ & (SOL_MARKDOWN_PREVIEW_LINE_RING - 1u)];
}

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
    SolBufferSystem    *system;
} TextClickCtx;

typedef struct ScrollbarDragCtx {
    SolUISystem     *ui;
    SolTextBuffer   *tb;
    SolBufferSystem *system;
    SolBufferNodeId  leaf_id;
    bool             is_vertical;
    int              max_scroll;
    float            track_len;
    float            thumb_len;
    float            grab_offset;
} ScrollbarDragCtx;

/* Count displayed monospace columns in the first `byte_len` bytes of `buf`.
   Causality renders tabs as four invisible space advances, so editor
   selection/caret geometry must use the same visual width. */
/*
 * Count the number of displayed monospace columns in the first byte_len bytes
 * of buf, expanding tabs to four columns.  UTF-8 multi-byte sequences are
 * treated as single columns.
 *
 * buf       Buffer to measure.
 * byte_len  Number of bytes to consider.
 * Returns   Visual column count.
 */
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
/*
 * Map a target visual column back to the corresponding codepoint column
 * boundary in buf.  Clicks inside a tab glyph are resolved to the nearest
 * editable boundary using half-width rounding.
 *
 * buf               Buffer to walk.
 * byte_len          Length of the buffer in bytes.
 * target_visual_col Target visual column to resolve.
 * Returns           Codepoint-column index.
 */
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

static SolTextBuffer   *g_scrollbar_drag_tb = NULL;
static SolBufferNodeId  g_scrollbar_drag_leaf_id = 0u;
static bool             g_scrollbar_drag_vertical = false;
static float            g_scrollbar_drag_grab_offset = 0.0f;
static bool             g_scrollbar_drag_active = false;

/* Return the next available per-frame line text slot from the ring buffer. */
static char *acquire_line_slot(void)
{
    return g_line_ring[g_line_ring_cursor++ & (SOL_TEXT_VIEW_LINE_RING - 1)];
}
/* Return the next available per-frame gutter line-number slot from the ring buffer. */
static char *acquire_num_slot(void)
{
    return g_num_ring[g_num_ring_cursor++ & (SOL_TEXT_VIEW_NUM_RING - 1)];
}
/* Return the next available per-frame click-context slot from the ring buffer. */
static TextClickCtx *acquire_click_slot(void)
{
    return &g_click_ring[g_click_ring_cursor++ & (SOL_TEXT_VIEW_CLICK_RING - 1)];
}
/* Return the next available per-frame scrollbar drag-context slot from the ring buffer. */
static ScrollbarDragCtx *acquire_scrollbar_slot(void)
{
    return &g_scrollbar_ring[
        g_scrollbar_ring_cursor++ & (SOL_TEXT_VIEW_SCROLLBAR_RING - 1)];
}

/* Emit a stable substring into the current Markdown block. */
static void markdown_emit_range(const char *text, size_t length, const char *style)
{
    if (!text || length == 0u) return;
    char *slot = acquire_markdown_preview_slot();
    if (length >= SOL_TEXT_VIEW_MAX_LINE_BYTES) length = SOL_TEXT_VIEW_MAX_LINE_BYTES - 1u;
    memcpy(slot, text, length);
    slot[length] = '\0';
    ca_text(&(Ca_TextDesc){ .text = slot, .style = style });
}

/* Render inline strong and code spans while retaining safe plain-text output. */
static void markdown_emit_inline(const char *text, const char *base_style)
{
    if (!text || !text[0]) {
        ca_text(&(Ca_TextDesc){ .text = " ", .style = base_style });
        return;
    }
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL });
    SolMarkdownInlineToken tokens[128];
    const size_t count = sol_markdown_inline_tokens(text, tokens, 128u);
    for (size_t i = 0u; i < count; ++i) {
        const char *style = base_style;
        switch (tokens[i].style) {
        case SOL_MARKDOWN_INLINE_EMPHASIS:      style = "markdown-emphasis"; break;
        case SOL_MARKDOWN_INLINE_STRONG:        style = "markdown-strong"; break;
        case SOL_MARKDOWN_INLINE_STRIKETHROUGH: style = "markdown-strikethrough"; break;
        case SOL_MARKDOWN_INLINE_CODE:          style = "markdown-inline-code"; break;
        case SOL_MARKDOWN_INLINE_LINK:          style = "markdown-link"; break;
        case SOL_MARKDOWN_INLINE_PLAIN:         break;
        }
        markdown_emit_range(text + tokens[i].start_byte,
                            tokens[i].byte_length, style);
    }
    ca_div_end();
}

#define SOL_MARKDOWN_TABLE_LEFT_INSET_COLUMNS 1u
#define SOL_MARKDOWN_TABLE_RIGHT_INSET_COLUMNS 2u
#define SOL_MARKDOWN_TABLE_FONT_SIZE_PX 13.0f

typedef struct MarkdownTableLayout {
    size_t column_count;
    float *column_widths;
    float cell_left_inset;
    float cell_right_inset;
} MarkdownTableLayout;

/* Return the number of non-empty structural cells in a pipe-delimited row. */
static size_t markdown_table_cell_count(const char *line)
{
    if (!line) return 0u;
    const char *cell = line[0] == '|' ? line + 1u : line;
    size_t count = 0u;
    while (*cell) {
        const char *end = strchr(cell, '|');
        const char *start = cell;
        const char *finish = end ? end : cell + strlen(cell);
        while (start < finish && (*start == ' ' || *start == '\t')) ++start;
        while (finish > start && (finish[-1] == ' ' || finish[-1] == '\t')) --finish;
        if (finish > start) ++count;
        if (!end) break;
        cell = end + 1u;
    }
    return count;
}

/* Find the first contiguous pipe-table row containing a source line. */
static size_t markdown_table_first_line(const SolTextBuffer *tb, size_t line_index)
{
    if (!tb) return line_index;
    char row[SOL_TEXT_VIEW_MAX_LINE_BYTES];
    while (line_index > 0u) {
        sol_text_buffer_copy_line(tb, line_index - 1u, row, sizeof(row));
        if (markdown_table_cell_count(row) < 2u) break;
        --line_index;
    }
    return line_index;
}

/* Release storage held by one measured table layout. */
static void markdown_table_layout_destroy(MarkdownTableLayout *layout)
{
    if (!layout) return;
    free(layout->column_widths);
    *layout = (MarkdownTableLayout){0};
}

/* Measure a Markdown cell using the same font size used by its text style. */
static float markdown_table_text_width(Ca_Window *window, const char *text,
                                       size_t text_length, float ui_scale,
                                       float fallback_glyph_width)
{
    if (!text || text_length == 0u) return 0.0f;
    if (text_length >= SOL_TEXT_VIEW_MAX_LINE_BYTES)
        text_length = SOL_TEXT_VIEW_MAX_LINE_BYTES - 1u;

    char measured[SOL_TEXT_VIEW_MAX_LINE_BYTES];
    memcpy(measured, text, text_length);
    measured[text_length] = '\0';
    const float measured_width = window
        ? ca_measure_text_px(window, measured, SOL_MARKDOWN_TABLE_FONT_SIZE_PX)
        : 0.0f;
    if (measured_width > 0.0f && ui_scale > 0.0f)
        return measured_width / ui_scale;
    return (float)tv_visual_col_count(text, text_length) * fallback_glyph_width;
}

/* Update table width requirements from one source row. */
static void markdown_table_measure_row(const char *line, size_t column_count,
                                       float *widths, Ca_Window *window,
                                       float ui_scale, float fallback_glyph_width)
{
    if (!line || column_count == 0u || !widths) return;
    const char *cell = line[0] == '|' ? line + 1u : line;
    size_t column = 0u;
    while (*cell && column < column_count) {
        const char *end = strchr(cell, '|');
        const char *start = cell;
        const char *finish = end ? end : cell + strlen(cell);
        while (start < finish && (*start == ' ' || *start == '\t')) ++start;
        while (finish > start && (finish[-1] == ' ' || finish[-1] == '\t')) --finish;
        if (finish > start) {
            const float width = markdown_table_text_width(
                window, start, (size_t)(finish - start), ui_scale,
                fallback_glyph_width);
            if (width > widths[column]) widths[column] = width;
        }
        ++column;
        if (!end) break;
        cell = end + 1u;
    }
}

/* Build one table-wide schema from every contiguous source row. */
static MarkdownTableLayout markdown_table_layout(const SolTextBuffer *tb,
                                                 size_t current_line,
                                                 Ca_Window *window,
                                                 float ui_scale,
                                                 float fallback_glyph_width)
{
    MarkdownTableLayout layout = {0};
    if (!tb) return layout;
    const size_t first = markdown_table_first_line(tb, current_line);
    char row[SOL_TEXT_VIEW_MAX_LINE_BYTES];
    const size_t total = sol_text_buffer_line_count(tb);
    for (size_t line = first; line < total; ++line) {
        sol_text_buffer_copy_line(tb, line, row, sizeof(row));
        if (markdown_table_cell_count(row) < 2u) break;
        const size_t columns = markdown_table_cell_count(row);
        if (columns > layout.column_count) layout.column_count = columns;
    }
    if (layout.column_count == 0u) return layout;

    layout.column_widths = calloc(layout.column_count, sizeof(*layout.column_widths));
    if (!layout.column_widths) {
        markdown_table_layout_destroy(&layout);
        return layout;
    }
    for (size_t line = first; line < total; ++line) {
        sol_text_buffer_copy_line(tb, line, row, sizeof(row));
        if (markdown_table_cell_count(row) < 2u) break;
        markdown_table_measure_row(row, layout.column_count, layout.column_widths,
                                   window, ui_scale, fallback_glyph_width);
    }
    layout.cell_left_inset = markdown_table_text_width(
        window, "M", SOL_MARKDOWN_TABLE_LEFT_INSET_COLUMNS, ui_scale,
        fallback_glyph_width);
    layout.cell_right_inset = markdown_table_text_width(
        window, "MM", SOL_MARKDOWN_TABLE_RIGHT_INSET_COLUMNS, ui_scale,
        fallback_glyph_width);
    for (size_t column = 0u; column < layout.column_count; ++column)
        layout.column_widths[column] += layout.cell_left_inset +
            layout.cell_right_inset;
    return layout;
}

/* Select header and alternating body treatments from source-table position. */
static const char *markdown_table_cell_style(const SolTextBuffer *tb,
                                             size_t line_index, bool separator)
{
    if (separator) return "markdown-table-separator-cell";
    const size_t first = markdown_table_first_line(tb, line_index);
    if (line_index == first) return "markdown-table-cell markdown-table-header-cell";
    return ((line_index - first) & 1u)
        ? "markdown-table-cell markdown-table-cell-alt"
        : "markdown-table-cell";
}

/* Return the full rendered width of the table containing a source row. */
static float markdown_table_width(const SolTextBuffer *tb, size_t line_index,
                                  Ca_Window *window, float ui_scale,
                                  float fallback_glyph_width)
{
    MarkdownTableLayout layout = markdown_table_layout(
        tb, line_index, window, ui_scale, fallback_glyph_width);
    if (layout.column_count == 0u) return 0.0f;

    float width = 2.0f * (float)(layout.column_count - 1u);
    for (size_t column = 0u; column < layout.column_count; ++column)
        width += layout.column_widths[column];
    markdown_table_layout_destroy(&layout);
    return width;
}

/* Measure rendered table rows in the viewport for native horizontal scrolling. */
static float markdown_visible_table_width(const SolTextBuffer *tb, int scroll_top,
                                          int rendered, Ca_Window *window,
                                          float ui_scale, float fallback_glyph_width)
{
    if (!tb || scroll_top < 0 || rendered <= 0) return 0.0f;

    SolMarkdownParserState state;
    sol_markdown_parser_init(&state);
    char line[SOL_TEXT_VIEW_MAX_LINE_BYTES];
    for (int index = 0; index < scroll_top; ++index) {
        sol_text_buffer_copy_line(tb, (size_t)index, line, sizeof(line));
        (void)sol_markdown_parse_line(line, &state);
    }

    const int total = (int)sol_text_buffer_line_count(tb);
    float width = 0.0f;
    for (int offset = 0; offset < rendered; ++offset) {
        const int index = scroll_top + offset;
        if (index >= total) break;
        sol_text_buffer_copy_line(tb, (size_t)index, line, sizeof(line));
        const SolMarkdownBlock block = sol_markdown_parse_line(line, &state);
        if (block.kind != SOL_MARKDOWN_BLOCK_TABLE &&
            block.kind != SOL_MARKDOWN_BLOCK_TABLE_SEPARATOR)
            continue;
        const float table_width = markdown_table_width(
            tb, (size_t)index, window, ui_scale, fallback_glyph_width);
        if (table_width > width) width = table_width;
    }
    return width;
}

/* Render one fixed-grid table row or alignment separator. */
static void markdown_emit_table_row(const char *line,
                                    const MarkdownTableLayout *layout,
                                    const SolTextBuffer *tb, size_t line_index,
                                    bool separator)
{
    const size_t count = layout ? layout->column_count : 0u;
    if (!line || count == 0u) return;
    const float gap = 2.0f;
    float table_width = gap * (float)(count - 1u);
    for (size_t column = 0u; column < count; ++column)
        table_width += layout->column_widths[column];
    const char *const cell_style = markdown_table_cell_style(tb, line_index,
                                                              separator);
    const char *cell = line[0] == '|' ? line + 1u : line;
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .width = table_width,
                                .height = (float)SOL_TEXT_LINE_HEIGHT_PX,
                                .style = "markdown-table-row" });
    size_t column = 0u;
    while (*cell && column < count) {
        const char *end = strchr(cell, '|');
        const char *trimmed_end = end ? end : cell + strlen(cell);
        while (trimmed_end > cell && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) --trimmed_end;
        while (*cell == ' ' || *cell == '\t') ++cell;
        if (trimmed_end <= cell) {
            if (!end) break;
            cell = end + 1u;
            continue;
        }
        ca_div_begin(&(Ca_DivDesc){ .width = layout->column_widths[column++],
            .height = (float)SOL_TEXT_LINE_HEIGHT_PX,
            .style = cell_style });
        if (!separator) {
        ca_div_begin(&(Ca_DivDesc){ .width = layout->cell_left_inset,
                                    .height = (float)SOL_TEXT_LINE_HEIGHT_PX });
        ca_div_end();
        char *cell_text = acquire_markdown_preview_slot();
        size_t cell_length = (size_t)(trimmed_end - cell);
        if (cell_length >= SOL_TEXT_VIEW_MAX_LINE_BYTES)
            cell_length = SOL_TEXT_VIEW_MAX_LINE_BYTES - 1u;
        memcpy(cell_text, cell, cell_length);
        cell_text[cell_length] = '\0';
        markdown_emit_inline(cell_text, "markdown-table");
        }
        ca_div_end();
        if (!end) break;
        cell = end + 1u;
        if (*cell == '\0') break;
    }
    ca_div_end();
}

/* Render a parsed Markdown block without changing the editor's row geometry. */
static void markdown_emit_block(const char *line, const SolMarkdownBlock *block,
                                const SolTextBuffer *tb, size_t line_index,
                                float line_width, Ca_Window *window,
                                float ui_scale, float fallback_glyph_width)
{
    if (!line || !block) return;
    const char *content = line + block->content_start_byte;
    switch (block->kind) {
    case SOL_MARKDOWN_BLOCK_HEADING:
        markdown_emit_inline(content, block->heading_level == 1u ? "markdown-heading-1" :
                                     block->heading_level == 2u ? "markdown-heading-2" : "markdown-heading-3"); break;
    case SOL_MARKDOWN_BLOCK_QUOTE: markdown_emit_inline(content, "markdown-quote"); break;
    case SOL_MARKDOWN_BLOCK_UNORDERED_LIST:
        ca_text(&(Ca_TextDesc){ .text = "• ", .style = "markdown-list-marker" });
        markdown_emit_inline(content, "markdown-list"); break;
    case SOL_MARKDOWN_BLOCK_ORDERED_LIST:
        ca_text(&(Ca_TextDesc){ .text = "# ", .style = "markdown-list-marker" });
        markdown_emit_inline(content, "markdown-list"); break;
    case SOL_MARKDOWN_BLOCK_TASK:
        ca_text(&(Ca_TextDesc){ .text = block->task_checked ? "☑ " : "☐ ", .style = "markdown-list-marker" });
        markdown_emit_inline(content, block->task_checked ? "markdown-task-done" : "markdown-list"); break;
    case SOL_MARKDOWN_BLOCK_FENCE:
        ca_text(&(Ca_TextDesc){ .text = block->fence_language && block->fence_language[0] ? block->fence_language : "code", .style = "markdown-fence" }); break;
    case SOL_MARKDOWN_BLOCK_CODE:
        ca_div_begin(&(Ca_DivDesc){ .position = CA_POSITION_ABSOLUTE, .pos_x = 0.0f,
            .pos_y = 0.0f, .width = line_width, .height = (float)SOL_TEXT_LINE_HEIGHT_PX,
            .style = "markdown-code-line" });
        ca_div_end();
        ca_text(&(Ca_TextDesc){ .text = line[0] ? line : " ", .style = "markdown-code" }); break;
    case SOL_MARKDOWN_BLOCK_RULE:
        ca_text(&(Ca_TextDesc){ .text = "────────────────────────", .style = "markdown-rule" }); break;
    case SOL_MARKDOWN_BLOCK_TABLE:
        { MarkdownTableLayout layout = markdown_table_layout(
              tb, line_index, window, ui_scale, fallback_glyph_width);
          markdown_emit_table_row(line, &layout, tb, line_index, false);
          markdown_table_layout_destroy(&layout); }
        break;
    case SOL_MARKDOWN_BLOCK_TABLE_SEPARATOR:
        { MarkdownTableLayout layout = markdown_table_layout(
              tb, line_index, window, ui_scale, fallback_glyph_width);
          markdown_emit_table_row(line, &layout, tb, line_index, true);
          markdown_table_layout_destroy(&layout); }
        break;
    case SOL_MARKDOWN_BLOCK_PARAGRAPH:
        markdown_emit_inline(line, "markdown-paragraph"); break;
    }
}

/* Per-frame token-segment storage.
 * Each highlighted line can emit multiple ca_text nodes (one per colored
 * token / plain gap).  They need stable memory through the frame, so we
 * use a secondary ring analogous to the line ring above. */
#define SOL_TEXT_VIEW_TOKEN_RING 2048u
#define SOL_TEXT_VIEW_TOKEN_MAX   512u
static char g_token_ring[SOL_TEXT_VIEW_TOKEN_RING][SOL_TEXT_VIEW_TOKEN_MAX];
static int  g_token_ring_cursor = 0;

/* Return the next available per-frame syntax-token slot from the ring buffer. */
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

/* Per-leaf blink state. Keyed by leaf_id rather than a single global so
 * multiple split panes (each with their own cursor position) don't
 * overwrite each other's "last moved" timestamp every frame — with a
 * single shared state, two panes showing different cursor positions each
 * saw the other's render as "the cursor just moved" every frame, so
 * neither caret ever blinked correctly. Linear-scan cache sized to the
 * same simultaneous-split-pane ceiling used elsewhere (SOL_UI_MAX_SPLIT_
 * CALLBACKS); leaf_id 0 (no leaf context, e.g. render called without a
 * pane geometry) falls back to slot 0 shared by all such callers, which
 * matches the single-pane behavior this function always had. */
#define SOL_CARET_STATE_SLOTS 64u
typedef struct CaretBlinkState {
    SolBufferNodeId leaf_id;
    uint64_t        last_move_ms;
    size_t          prev_line;
    size_t          prev_col;
    bool            in_use;
} CaretBlinkState;
static CaretBlinkState g_caret_states[SOL_CARET_STATE_SLOTS];

/* Return the current monotonic time in milliseconds. */
static uint64_t monotonic_ms(void)
{
    return sol_platform_now_monotonic_ns() / 1000000ull;
}

/* Find (or claim) this leaf's blink-state slot. Falls back to the first
 * slot when the cache is full (extreme split counts) rather than growing
 * unbounded — the caret simply blinks relative to whichever leaf most
 * recently claimed that slot, a graceful degradation, not a crash. */
static CaretBlinkState *caret_state_for_leaf(SolBufferNodeId leaf_id)
{
    int free_slot = -1;
    for (size_t i = 0u; i < SOL_CARET_STATE_SLOTS; ++i) {
        if (g_caret_states[i].in_use && g_caret_states[i].leaf_id == leaf_id) {
            return &g_caret_states[i];
        }
        if (free_slot < 0 && !g_caret_states[i].in_use) free_slot = (int)i;
    }
    CaretBlinkState *slot = &g_caret_states[free_slot >= 0 ? (size_t)free_slot : 0u];
    slot->leaf_id       = leaf_id;
    slot->last_move_ms  = 0u;
    slot->prev_line     = (size_t)-1;
    slot->prev_col      = (size_t)-1;
    slot->in_use        = true;
    return slot;
}

/* Call once per frame with the current cursor position.  Returns true
 * when the caret should be drawn (on-phase or just moved). */
/*
 * Determine whether the caret should be visible this frame.  Resets the blink
 * phase when the cursor position changes and holds the caret solid for
 * SOL_CARET_SOLID_MS after any movement.
 *
 * leaf_id   Identifies which pane this caret belongs to, so multiple panes
 *           each track their own blink phase independently.
 * cur_line  Current cursor line index.
 * cur_col   Current cursor column index.
 * Returns   true when the caret should be drawn.
 */
static bool caret_blink_visible(SolBufferNodeId leaf_id, size_t cur_line, size_t cur_col)
{
    CaretBlinkState *st = caret_state_for_leaf(leaf_id);
    const uint64_t now = monotonic_ms();
    if (cur_line != st->prev_line || cur_col != st->prev_col) {
        st->prev_line    = cur_line;
        st->prev_col     = cur_col;
        st->last_move_ms = now;
    }
    /* Always solid immediately after movement. */
    if (now - st->last_move_ms < SOL_CARET_SOLID_MS) return true;
    /* Periodic blink phase relative to last move. */
    const uint64_t phase = (now - st->last_move_ms)
                           % (SOL_CARET_BLINK_HALF_MS * 2u);
    return phase < SOL_CARET_BLINK_HALF_MS;
}

/* ------------------------------------------------------------------ */
/* Syntax-highlight helpers                                            */
/* ------------------------------------------------------------------ */

/* Emit ca_text nodes for a single line using the pre-queried span list.
 * Spans must be sorted by start_byte and already clipped/filtered to
 * the range [line_start_byte, line_start_byte + line_bytes). */
/*
 * Emit Causality text nodes for a single line using pre-queried syntax spans.
 * Interleaves plain-text runs and coloured token runs from the span list,
 * writing each token into a per-frame ring-buffer slot.
 *
 * line_buf         The line content buffer.
 * line_bytes       Number of bytes in the line.
 * line_start_byte  Byte offset of the line's start within the file/rope.
 * spans            Sorted array of syntax spans clipped to this line's range.
 * span_count       Number of valid entries in spans.
 */
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

/*
 * Estimate the number of visible text lines for a given window height,
 * subtracting approximate UI chrome (title bar, tabs, status bar).
 *
 * window_h  Full window height in logical pixels.
 * ui_scale  Current UI scale factor.
 * Returns   Estimated visible line count.
 */
int sol_text_view_visible_lines(int window_h, float ui_scale)
{
    if (ui_scale <= 0.0f) ui_scale = 1.0f;
    return sol_text_view_visible_lines_for_height(
        (float)window_h - (float)SOL_TEXT_PANE_CHROME_PX * ui_scale,
        ui_scale);
}

/*
 * Compute how many lines to render for a pane of the given height.  Adds two
 * extra rows so the pane always appears fully filled even when the chrome
 * estimate is slightly off.
 *
 * pane_h    Pane content height in logical pixels (excluding chrome).
 * ui_scale  Current UI scale factor.
 * Returns   Number of lines to render (always >= 1).
 */
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

/*
 * Compute the number of visible monospace columns for a given pane width,
 * accounting for the gutter and scrollbar.
 *
 * pane_w                  Pane width in logical pixels.
 * ui_scale                Current UI scale factor.
 * glyph_advance_layout_px Monospace glyph advance in layout pixels.
 * Returns                 Number of visible columns (always >= 1).
 */
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
/*
 * Return the monospace glyph advance width in layout pixels for the given
 * window.  Falls back to 60% of the boot font size when the measurement is
 * unavailable.
 *
 * win     The Causality window to query (may be NULL).
 * Returns Glyph advance in layout pixels.
 */
static float glyph_advance_px_for(Ca_Window *win)
{
    float w = win ? ca_measure_text_px(win, "M", SOL_UI_BOOT_FONT_SIZE_PX_FLOAT)
                  : 0.0f;
    if (w <= 0.0f) w = SOL_UI_BOOT_FONT_SIZE_PX_FLOAT * 0.6f;
    return w;
}

/*
 * Find the widest visible line (in monospace columns) within the rendered
 * window, used to size the horizontal scrollbar thumb.
 *
 * tb          Text buffer to query.
 * scroll_top  Index of the first visible line.
 * rendered    Number of lines to check.
 * Returns     Maximum column width of any visible line.
 */
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

/*
 * Convert text-column-local pointer coordinates to a (line, codepoint column)
 * buffer position, accounting for scroll offset, line height, and glyph
 * advance.
 *
 * ui              The UI system providing the primary window for font metrics.
 * tb              The text buffer being hit-tested.
 * event_local_x   X coordinate in the text column's local space.
 * event_local_y   Y coordinate in the text column's local space.
 * out_line        Receives the resolved buffer line index.
 * out_cp_col      Receives the resolved codepoint column index.
 * Returns         true on success.
 */
static bool tv_local_to_line_col_ex(SolUISystem *ui, SolTextBuffer *tb,
                                    SolBufferSystem *bsystem,
                                    SolBufferNodeId bleaf_id,
                                    float event_local_x, float event_local_y,
                                    size_t *out_line, size_t *out_cp_col)
{
    if (!ui || !tb || !out_line || !out_cp_col) {
        return false;
    }

    Ca_Window *win = sol_ui_system_primary_window(ui);
    const float scale = sol_ui_system_scale(ui);
    const float pad_x = 8.0f * scale;
    const float pad_y = 8.0f * scale;
    float local_x = event_local_x - pad_x;
    float local_y = event_local_y - pad_y;
    if (local_x < 0.0f) local_x = 0.0f;
    if (local_y < 0.0f) local_y = 0.0f;

    const bool use_leaf = (bsystem != NULL && bleaf_id != 0u);
    const float line_h = (float)SOL_TEXT_LINE_HEIGHT_PX * scale;
    const int   row    = (int)(local_y / line_h);
    const int   scroll_top = use_leaf
        ? sol_buffer_leaf_scroll_top(bsystem, bleaf_id)
        : sol_text_buffer_scroll_top(tb);
    int line_idx = scroll_top + row;
    if (line_idx < 0) line_idx = 0;
    const int total = (int)sol_text_buffer_line_count(tb);
    if (total <= 0) {
        *out_line = 0u;
        *out_cp_col = 0u;
        return true;
    }
    if (line_idx >= total) line_idx = total - 1;

    const int scroll_left = use_leaf
        ? sol_buffer_leaf_scroll_left(bsystem, bleaf_id)
        : sol_text_buffer_scroll_left(tb);
    const float adv = glyph_advance_px_for(win);
    int visual_col = scroll_left + (int)((local_x / adv) + 0.5f);
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

static bool tv_local_to_line_col(SolUISystem *ui, SolTextBuffer *tb,
                                 float event_local_x, float event_local_y,
                                 size_t *out_line, size_t *out_cp_col)
{
    return tv_local_to_line_col_ex(ui, tb, NULL, 0u,
                                   event_local_x, event_local_y,
                                   out_line, out_cp_col);
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
/*
 * Convert pane-local drag-event coordinates to a (line, codepoint column)
 * pair, storing results as ints.
 *
 * ev          The Causality drag event with local coordinates.
 * cb          Click context providing the UI system and text buffer.
 * out_line    Receives the resolved line index.
 * out_cp_col  Receives the resolved codepoint column index.
 */
static void tv_ev_to_line_col(const Ca_DragEvent *ev, const TextClickCtx *cb,
                               int *out_line, int *out_cp_col)
{
    size_t line = 0u;
    size_t cp_col = 0u;
    if (!tv_local_to_line_col_ex(cb->ui, cb->tb, cb->system, cb->leaf_id,
                                  ev->local_x, ev->local_y,
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
/*
 * Handle the start of a drag (click) on the text column: focus the leaf pane,
 * move the cursor to the click position, and set the selection anchor so a
 * subsequent drag can extend the selection.
 */
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
/*
 * Handle mouse movement while the button is held over the text column.
 * Extends the selection from the anchor set during drag-start to the current
 * pointer position.
 */
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

/*
 * Handle the start of a scrollbar thumb drag: compute the grab offset so the
 * thumb tracks the pointer smoothly, and latch the drag state into the global
 * drag variables for use by subsequent on_scrollbar_drag calls.
 *
 * The offset is stored as a fraction of thumb length (0..1), not an absolute
 * pixel distance: a resize or ui_scale change mid-drag rebuilds the pane with
 * a different track_len/thumb_len every frame, and a pixel offset captured
 * against the old geometry would apply the wrong grab point (and can exceed
 * the new thumb_len entirely) once combined with the new one.
 */
static void on_scrollbar_drag_start(const Ca_DragEvent *ev, void *user_data)
{
    ScrollbarDragCtx *ctx = (ScrollbarDragCtx *)user_data;
    if (!ev || !ctx || !ctx->tb) return;

    const bool has_leaf = (ctx->system != NULL && ctx->leaf_id != 0u);
    const int current = ctx->is_vertical
        ? (has_leaf ? sol_buffer_leaf_scroll_top(ctx->system, ctx->leaf_id)
                    : sol_text_buffer_scroll_top(ctx->tb))
        : (has_leaf ? sol_buffer_leaf_scroll_left(ctx->system, ctx->leaf_id)
                    : sol_text_buffer_scroll_left(ctx->tb));
    const float travel = ctx->track_len - ctx->thumb_len;
    const float pct = (ctx->max_scroll > 0 && travel > 0.0f)
        ? (float)current / (float)ctx->max_scroll : 0.0f;
    const float thumb_pos = pct * travel;
    const float pointer = ctx->is_vertical ? ev->local_y : ev->local_x;

    float grab_frac;
    if (ctx->thumb_len > 0.0f &&
        pointer >= thumb_pos && pointer <= thumb_pos + ctx->thumb_len) {
        grab_frac = (pointer - thumb_pos) / ctx->thumb_len;
    } else {
        grab_frac = 0.5f;
    }
    if (grab_frac < 0.0f) grab_frac = 0.0f;
    if (grab_frac > 1.0f) grab_frac = 1.0f;
    ctx->grab_offset = grab_frac;
    g_scrollbar_drag_tb = ctx->tb;
    g_scrollbar_drag_leaf_id = ctx->leaf_id;
    g_scrollbar_drag_vertical = ctx->is_vertical;
    g_scrollbar_drag_grab_offset = grab_frac;
    g_scrollbar_drag_active = true;
}

/*
 * Handle scrollbar thumb movement: convert the pointer position to a scroll
 * value and update the text buffer's scroll_top or scroll_left accordingly.
 */
static void on_scrollbar_drag(const Ca_DragEvent *ev, void *user_data)
{
    ScrollbarDragCtx *ctx = (ScrollbarDragCtx *)user_data;
    if (!ev || !ctx || !ctx->tb || ctx->max_scroll <= 0) return;

    const float travel = ctx->track_len - ctx->thumb_len;
    if (travel <= 0.0f) return;

    const float pointer = ctx->is_vertical ? ev->local_y : ev->local_x;
    float grab_frac = ctx->grab_offset;
    if (g_scrollbar_drag_active &&
        (g_scrollbar_drag_leaf_id == ctx->leaf_id ||
         g_scrollbar_drag_tb == ctx->tb) &&
        g_scrollbar_drag_vertical == ctx->is_vertical) {
        grab_frac = g_scrollbar_drag_grab_offset;
    }
    /* grab_frac is a 0..1 fraction of thumb length, re-applied against this
     * frame's (possibly resized) thumb_len so mid-drag layout changes don't
     * desync the grab point from the pointer. */
    const float grab_offset = grab_frac * ctx->thumb_len;
    float pct = (pointer - grab_offset) / travel;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    int scroll = (int)(pct * (float)ctx->max_scroll + 0.5f);
    if (scroll < 0) scroll = 0;
    if (scroll > ctx->max_scroll) scroll = ctx->max_scroll;

    const bool has_leaf = (ctx->system != NULL && ctx->leaf_id != 0u);
    if (ctx->is_vertical) {
        if (has_leaf)
            sol_buffer_set_leaf_scroll_top(ctx->system, ctx->leaf_id, scroll);
        else
            sol_text_buffer_set_scroll_top(ctx->tb, scroll);
    } else {
        if (has_leaf)
            sol_buffer_set_leaf_scroll_left(ctx->system, ctx->leaf_id, scroll);
        else
            sol_text_buffer_set_scroll_left(ctx->tb, scroll);
    }
    if (ctx->ui) sol_ui_system_invalidate_buffer_area(ctx->ui);
}

/* Clear the global scrollbar drag state when the pointer button is released. */
static void on_scrollbar_drag_end(const Ca_DragEvent *ev, void *user_data)
{
    (void)ev;
    (void)user_data;
    g_scrollbar_drag_tb = NULL;
    g_scrollbar_drag_leaf_id = 0u;
    g_scrollbar_drag_vertical = false;
    g_scrollbar_drag_grab_offset = 0.0f;
    g_scrollbar_drag_active = false;
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

/*
 * Render one text buffer pane into the current Causality frame context.
 * Emits the gutter (line numbers), the text column (highlighted content,
 * selection overlay, caret), an optional horizontal scrollbar, and a vertical
 * scrollbar when content overflows.
 *
 * buffer  The buffer whose content is rendered.
 * args    Per-pane render arguments: geometry rect, leaf ID, active flag.
 * state   Pointer to the SolTextBuffer holding scroll and cursor state.
 */
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
    const float ui_scale = sol_ui_system_scale(ui);
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
    /* Use per-leaf scroll when a leaf id and system are available so that
     * multiple panes showing the same buffer each track their own position. */
    SolBufferSystem *bsys = args ? (SolBufferSystem *)args->system : NULL;
    const SolBufferNodeId leaf_id = args ? args->leaf_id : 0u;
    const bool has_leaf_scroll = (bsys != NULL && leaf_id != 0u);
    int scroll_top = has_leaf_scroll
        ? sol_buffer_leaf_scroll_top(bsys, leaf_id)
        : sol_text_buffer_scroll_top(tb);
    if (scroll_top > max_top) scroll_top = max_top;
    if (scroll_top < 0)       scroll_top = 0;
    if (has_leaf_scroll) {
        if (scroll_top != sol_buffer_leaf_scroll_top(bsys, leaf_id))
            sol_buffer_set_leaf_scroll_top(bsys, leaf_id, scroll_top);
    } else if (scroll_top != sol_text_buffer_scroll_top(tb)) {
        sol_text_buffer_set_scroll_top(tb, scroll_top);
    }

    const size_t cur_line = sol_text_buffer_cursor_line(tb);
    const size_t cur_col  = sol_text_buffer_cursor_col(tb);
    const float adv_css = glyph_advance_px_for(primary_win) / ui_scale;
    const int viewport_cols = sol_text_view_visible_cols_for_width(
        args ? args->rect.w : 0.0f, ui_scale, adv_css * ui_scale);
    size_t max_line_cols = visible_max_line_cols(tb, scroll_top, rendered);
    if (sol_text_buffer_is_markdown_document(tb) && adv_css > 0.0f) {
        const float table_width = markdown_visible_table_width(
            tb, scroll_top, rendered, primary_win, ui_scale, adv_css);
        const size_t table_columns = (size_t)((table_width + adv_css - 0.001f) /
                                              adv_css);
        if (table_columns > max_line_cols) max_line_cols = table_columns;
    }
    const int max_left = max_line_cols > (size_t)viewport_cols
        ? (int)(max_line_cols - (size_t)viewport_cols) : 0;
    int scroll_left = has_leaf_scroll
        ? sol_buffer_leaf_scroll_left(bsys, leaf_id)
        : sol_text_buffer_scroll_left(tb);
    if (scroll_left > max_left) scroll_left = max_left;
    if (scroll_left < 0) scroll_left = 0;
    if (has_leaf_scroll) {
        if (scroll_left != sol_buffer_leaf_scroll_left(bsys, leaf_id))
            sol_buffer_set_leaf_scroll_left(bsys, leaf_id, scroll_left);
    } else if (scroll_left != sol_text_buffer_scroll_left(tb)) {
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
        .style     = "buffer-scroll-row workspace-panel-well",
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
    cb->system  = bsys;

    ca_div_begin(&(Ca_DivDesc){
        .direction     = CA_VERTICAL,
        .style         = "buffer-text-col",
        .on_drag_start = ui ? on_text_col_drag_start : NULL,
        .on_drag       = ui ? on_text_col_drag_move  : NULL,
        .drag_data     = cb,
    });

    /* Pane-height column rulers are background overlays, independent of line
     * nodes and text content. Horizontal scrolling moves them with columns. */
    for (size_t ruler = 0u;
         ruler < sizeof(SOL_TEXT_RULER_COLUMNS) /
                     sizeof(SOL_TEXT_RULER_COLUMNS[0]);
         ruler++) {
        ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE,
            .pos_x    = (float)SOL_TEXT_RULER_COLUMNS[ruler] * adv_css
                        - scroll_x,
            .pos_y    = 0.0f,
            .width    = 1.0f,
            .height   = (float)rendered * SOL_TEXT_LINE_HEIGHT_PX,
            .style    = "buffer-column-ruler",
        });
        ca_div_end();
    }

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
    const bool markdown_document = sol_text_buffer_is_markdown_document(tb);
    SolMarkdownParserState markdown_state;
    sol_markdown_parser_init(&markdown_state);
    if (markdown_document) {
        for (int line = 0; line < scroll_top; ++line) {
            char prior[SOL_TEXT_VIEW_MAX_LINE_BYTES];
            sol_text_buffer_copy_line(tb, (size_t)line, prior, sizeof(prior));
            (void)sol_markdown_parse_line(prior, &markdown_state);
        }
    }
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
        SolMarkdownBlock markdown_block = {0};
        if (markdown_document)
            markdown_block = sol_markdown_parse_line(line_buf, &markdown_state);

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
                    .style      = "buffer-selection",
                });
                ca_div_end();
            }
        }

        /* Markdown keeps the normal editor's row geometry and input path.
           Only inactive lines swap their source markers for presentation. */
        if (markdown_document && !is_cursor_line) {
            markdown_emit_block(line_buf, &markdown_block, tb, (size_t)line_idx,
                                line_content_w, primary_win, ui_scale, adv_css);
        } else {
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
            const bool  visible = caret_blink_visible(
                args ? args->leaf_id : 0u, cur_line, cur_col);

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
                .style = visible ? "buffer-caret" : "buffer-caret buffer-caret-hidden",
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
        hctx->system = bsys;
        hctx->leaf_id = leaf_id;
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
        vctx->system = bsys;
        vctx->leaf_id = leaf_id;
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
