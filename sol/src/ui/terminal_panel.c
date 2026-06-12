// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* terminal_panel.c — Causality rendering for the integrated terminal panel.
 *
 * Renders a tab strip at the top of the panel and a scrollable cell grid
 * beneath it.  The cell grid walks each visible row, groups consecutive cells
 * with identical SGR attributes into runs, and emits one ca_div_begin +
 * ca_text per run so the GPU never overdraws.
 *
 * Click handling for the viewport sets terminal focus; the tab strip has
 * per-tab click contexts so clicking a tab switches the active terminal.
 */

#include "sol_ui_internal.h"

#include <causality.h>

#include <stdint.h>
#include <string.h>

/* ================================================================== */
/* UTF-8 encoding                                                      */
/* ================================================================== */

/*
 * Encode a Unicode codepoint as UTF-8 into `buf`.
 * `buf` must have room for at least 4 bytes + NUL (5 bytes total).
 * Returns the number of bytes written (excluding the NUL terminator).
 *
 * cp   Unicode codepoint.
 * buf  Output buffer (at least 5 bytes).
 */
static int encode_utf8(uint32_t cp, char *buf)
{
    if (cp < 0x80u) {
        buf[0] = (char)cp;
        buf[1] = '\0';
        return 1;
    } else if (cp < 0x800u) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        buf[2] = '\0';
        return 2;
    } else if (cp < 0x10000u) {
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        buf[3] = '\0';
        return 3;
    } else {
        buf[0] = (char)(0xF0u | (cp >> 18));
        buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (char)(0x80u | (cp & 0x3Fu));
        buf[4] = '\0';
        return 4;
    }
}

/* ================================================================== */
/* Click contexts                                                      */
/* ================================================================== */

/* Per-tab click context — stable within a frame since it's on the stack.
   Causality's reactive runtime keeps button nodes alive between frames;
   the click fires synchronously on the main thread so stack lifetime is fine. */
typedef struct TermTabClickCtx {
    SolUISystem *ui;
    size_t       tab_index;
} TermTabClickCtx;

static TermTabClickCtx g_term_tab_ctxs[SOL_TERM_MAX_TABS];

typedef struct TermViewportClickCtx {
    SolUISystem *ui;
} TermViewportClickCtx;

static TermViewportClickCtx g_term_viewport_ctx;

/* Per-tab close-button click context — parallel array to g_term_tab_ctxs. */
typedef struct TermTabCloseCtx {
    SolUISystem *ui;
    size_t       tab_index;
} TermTabCloseCtx;

static TermTabCloseCtx g_term_tab_close_ctxs[SOL_TERM_MAX_TABS];

static void on_term_tab_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    TermTabClickCtx *ctx = (TermTabClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->terminal_mgr) return;
    SolTerminalManager *mgr = ctx->ui->terminal_mgr;
    /* Switch active tab. */
    while (sol_terminal_manager_active_index(mgr) != ctx->tab_index) {
        if (sol_terminal_manager_active_index(mgr) < ctx->tab_index)
            sol_terminal_manager_next_tab(mgr);
        else
            sol_terminal_manager_prev_tab(mgr);
    }
    sol_terminal_manager_set_focused(mgr, true);
    sol_ui_system_terminal_notify(ctx->ui);
}

/*
 * Close the terminal tab at ctx->tab_index.
 * Switches active index to the target tab then closes it via the manager,
 * which handles PTY kill and memory cleanup internally.
 *
 * btn        Unused Ca_Button pointer.
 * user_data  TermTabCloseCtx* identifying which tab to close.
 */
static void on_term_tab_close(Ca_Button *btn, void *user_data)
{
    (void)btn;
    TermTabCloseCtx *ctx = (TermTabCloseCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->terminal_mgr) return;
    SolTerminalManager *mgr = ctx->ui->terminal_mgr;
    /* Navigate to the target tab before closing — close_active always closes the active one. */
    while (sol_terminal_manager_active_index(mgr) != ctx->tab_index) {
        if (sol_terminal_manager_active_index(mgr) < ctx->tab_index)
            sol_terminal_manager_next_tab(mgr);
        else
            sol_terminal_manager_prev_tab(mgr);
    }
    sol_terminal_manager_close_active(mgr);
    sol_ui_system_terminal_notify(ctx->ui);
}

static void on_term_viewport_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    TermViewportClickCtx *ctx = (TermViewportClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->terminal_mgr) return;
    SolTerminalManager *mgr = ctx->ui->terminal_mgr;
    if (!sol_terminal_manager_focused(mgr)) {
        sol_terminal_manager_set_focused(mgr, true);
        sol_ui_system_terminal_notify(ctx->ui);
    }
}

/* ================================================================== */
/* Cell attribute comparison                                           */
/* ================================================================== */

/* Two cells belong to the same run if their visual attributes match.
   Only compares fg, bg, and relevant attr flags — codepoint is excluded. */
static bool cells_same_run(const SolTermCell *a, const SolTermCell *b,
                            bool a_is_cursor, bool b_is_cursor)
{
    if (a_is_cursor != b_is_cursor) return false;
    if (a->fg.mode != b->fg.mode)   return false;
    if (a->bg.mode != b->bg.mode)   return false;
    if (a->attrs   != b->attrs)     return false;
    switch (a->fg.mode) {
    case SOL_TERM_COLOR_INDEXED: if (a->fg.index != b->fg.index) return false; break;
    case SOL_TERM_COLOR_RGB:
        if (a->fg.rgb.r != b->fg.rgb.r || a->fg.rgb.g != b->fg.rgb.g ||
            a->fg.rgb.b != b->fg.rgb.b) return false;
        break;
    default: break;
    }
    switch (a->bg.mode) {
    case SOL_TERM_COLOR_INDEXED: if (a->bg.index != b->bg.index) return false; break;
    case SOL_TERM_COLOR_RGB:
        if (a->bg.rgb.r != b->bg.rgb.r || a->bg.rgb.g != b->bg.rgb.g ||
            a->bg.rgb.b != b->bg.rgb.b) return false;
        break;
    default: break;
    }
    return true;
}

/* ================================================================== */
/* Row rendering                                                       */
/* ================================================================== */

/* Maximum UTF-8 bytes per run: up to 256 cells × 4 bytes + NUL. */
#define TERM_RUN_BUF_SIZE 1056

/*
 * Render one terminal row as a horizontal div of attribute-grouped runs.
 *
 * term        The terminal.
 * row         Zero-based visual row index (0 = topmost visible row).
 * cursor_col  Column of the cursor (-1 if cursor not on this row).
 * focused     Whether the terminal panel has keyboard focus.
 */
static void render_term_row(const SolTerminal *term, int row,
                            int cursor_col, bool focused)
{
    const SolTermLine *line = sol_terminal_view_line(term, row);

    ca_div_begin(&(Ca_DivDesc){
        .direction  = CA_HORIZONTAL,
        .style      = "term-line",
        .background = 0u,   /* inherits term-panel background */
    });

    if (!line || !line->cells) {
        ca_div_end();
        return;
    }

    const int cols = line->cols;
    char run_buf[TERM_RUN_BUF_SIZE];
    int  run_len = 0;

    /* Determine effective fg/bg for the current run; rendered at run end. */
    uint32_t run_fg = 0u, run_bg = 0u;
    const char *run_style = "term-cell";
    bool run_started = false;
    bool prev_cursor = false;

    for (int c = 0; c < cols; ++c) {
        const SolTermCell *cell     = &line->cells[c];
        const bool         is_cursor = (c == cursor_col);

        /* Determine visual fg/bg, accounting for REVERSE and cursor. */
        SolTermColor eff_fg = cell->fg;
        SolTermColor eff_bg = cell->bg;
        if (cell->attrs & SOL_TERM_ATTR_REVERSE) {
            SolTermColor tmp = eff_fg;
            eff_fg = eff_bg;
            eff_bg = tmp;
        }

        uint32_t cell_fg, cell_bg;
        if (is_cursor) {
            /* Focused cursor: solid inverted block; unfocused: hollow (border). */
            if (focused) {
                cell_fg = sol_term_color_to_rgba(&eff_bg, false);
                cell_bg = sol_term_color_to_rgba(&eff_fg, true);
            } else {
                cell_fg = sol_term_color_to_rgba(&eff_fg, true);
                cell_bg = 0u;  /* transparent — border supplied by CSS class */
            }
        } else {
            cell_fg = sol_term_color_to_rgba(&eff_fg, true);
            cell_bg = sol_term_color_to_rgba(&eff_bg, false);
            if (cell_bg == sol_term_color_to_rgba(&(SolTermColor){ .mode = SOL_TERM_COLOR_DEFAULT }, false)) {
                cell_bg = 0u;   /* default bg → transparent (no overdraw) */
            }
        }

        const char *cell_style;
        if (is_cursor) {
            cell_style = focused ? "term-cursor-focused" : "term-cursor-unfocused";
        } else {
            const bool bold   = (cell->attrs & SOL_TERM_ATTR_BOLD)   != 0u;
            const bool italic = (cell->attrs & SOL_TERM_ATTR_ITALIC)  != 0u;
            if (bold && italic) cell_style = "term-cell-bold-italic";
            else if (bold)      cell_style = "term-cell-bold";
            else if (italic)    cell_style = "term-cell-italic";
            else                cell_style = "term-cell";
        }

        /* Decide whether this cell continues the current run or starts a new one. */
        const bool new_run = !run_started ||
                             is_cursor != prev_cursor ||
                             cell_fg != run_fg ||
                             cell_bg != run_bg ||
                             cell_style != run_style ||
                             run_len >= TERM_RUN_BUF_SIZE - 6;

        if (new_run && run_started) {
            /* Flush previous run. */
            run_buf[run_len] = '\0';
            ca_div_begin(&(Ca_DivDesc){
                .direction  = CA_HORIZONTAL,
                .background = run_bg,
            });
            ca_text(&(Ca_TextDesc){
                .text  = run_buf,
                .style = run_style,
                .color = run_fg,
            });
            ca_div_end();
            run_len = 0;
        }

        if (new_run || !run_started) {
            run_fg      = cell_fg;
            run_bg      = cell_bg;
            run_style   = cell_style;
            run_started = true;
        }
        prev_cursor = is_cursor;

        /* Append this cell's codepoint to the run buffer. */
        uint32_t cp = cell->codepoint;
        if (cp == 0u) cp = (uint32_t)' ';
        if (cell->attrs & SOL_TERM_ATTR_INVISIBLE) cp = (uint32_t)' ';
        char glyphbuf[6];
        int n = encode_utf8(cp, glyphbuf);
        if (run_len + n < TERM_RUN_BUF_SIZE - 1) {
            memcpy(run_buf + run_len, glyphbuf, (size_t)n);
            run_len += n;
        }
    }

    /* Flush final run. */
    if (run_started && run_len > 0) {
        run_buf[run_len] = '\0';
        ca_div_begin(&(Ca_DivDesc){
            .direction  = CA_HORIZONTAL,
            .background = run_bg,
        });
        ca_text(&(Ca_TextDesc){
            .text  = run_buf,
            .style = run_style,
            .color = run_fg,
        });
        ca_div_end();
    }

    ca_div_end();   /* term-line */
}

/* ================================================================== */
/* Public render entry point                                           */
/* ================================================================== */

/*
 * Render the terminal panel into the currently-open causality div.
 * Emits a tab strip (term-header) and a viewport (term-viewport) that
 * iterates visible rows of the active terminal.
 *
 * ui  The UI system (terminal_mgr must be non-NULL and visible).
 */
void sol_ui_render_terminal_panel(SolUISystem *ui)
{
    SolTerminalManager *mgr  = ui->terminal_mgr;
    const size_t        count = sol_terminal_manager_count(mgr);
    const size_t        active_idx = sol_terminal_manager_active_index(mgr);
    const bool          focused    = sol_terminal_manager_focused(mgr);

    /* ---- Tab strip ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "term-header",
    });
    for (size_t i = 0u; i < count; ++i) {
        const SolTerminal *t      = sol_terminal_manager_at(mgr, i);
        const bool         active = (i == active_idx);
        const char        *title  = t ? sol_terminal_title(t) : "Terminal";

        g_term_tab_ctxs[i].ui        = ui;
        g_term_tab_ctxs[i].tab_index = i;

        g_term_tab_close_ctxs[i].ui        = ui;
        g_term_tab_close_ctxs[i].tab_index = i;

        ca_btn_begin(&(Ca_BtnDesc){
            .style      = active ? "term-tab-active" : "term-tab",
            .direction  = CA_HORIZONTAL,
            .background = 0u,
            .on_click   = on_term_tab_click,
            .click_data = &g_term_tab_ctxs[i],
        });
        ca_text(&(Ca_TextDesc){
            .text  = title,
            .style = active ? "term-tab-text-active" : "term-tab-text",
        });
        ca_btn_begin(&(Ca_BtnDesc){
            .style      = "term-tab-close",
            .background = 0u,
            .on_click   = on_term_tab_close,
            .click_data = &g_term_tab_close_ctxs[i],
        });
        ca_text(&(Ca_TextDesc){
            .text  = "\xC3\x97",   /* UTF-8 for U+00D7 MULTIPLICATION SIGN (×) */
            .style = active ? "term-tab-close-icon-active" : "term-tab-close-icon",
        });
        ca_btn_end();  /* term-tab-close */
        ca_btn_end();  /* term-tab / term-tab-active */
    }
    ca_div_end();   /* term-header */

    /* ---- Viewport (clickable to claim focus) ---- */
    g_term_viewport_ctx.ui = ui;
    ui->term_viewport_host = ca_btn_begin(&(Ca_BtnDesc){
        .style      = "term-viewport",
        .direction  = CA_VERTICAL,
        .background = 0u,
        .on_click   = on_term_viewport_click,
        .click_data = &g_term_viewport_ctx,
    });

    SolTerminal *term = sol_terminal_manager_active(mgr);
    if (term) {
        const int  rows        = sol_terminal_rows(term);
        const int  cursor_row  = sol_terminal_cursor_row(term);
        const int  cursor_col  = sol_terminal_cursor_col(term);
        const int  view_scroll = sol_terminal_view_scroll(term);
        /* Cursor visible only when DECTCEM is set, scroll is at bottom,
           and the blink phase is on (toggled at 530 ms by on_frame). */
        const bool cur_vis = sol_terminal_cursor_visible(term)
                             && ui->term_cursor_blink_on;

        for (int r = 0; r < rows; ++r) {
            const bool cursor_on_row = cur_vis && (view_scroll == 0) &&
                                       (r == cursor_row);
            render_term_row(term, r, cursor_on_row ? cursor_col : -1, focused);
        }
    }

    /* Filler absorbs the fractional pixel remainder that integer row-count
       truncation leaves below the last rendered row. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "term-filler",
    });
    ca_div_end();

    ca_btn_end();   /* term-viewport */
}
