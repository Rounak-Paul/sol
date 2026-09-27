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
#include <stdio.h>
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
   the click fires synchronously on the main thread so stack lifetime is fine.
   Shared by both the tab-select and tab-close buttons: the two actions
   need identical addressing (which tab), so one context/array pair covers
   both — g_term_tab_close_ctxs used to duplicate this verbatim. */
typedef struct TermTabClickCtx {
    SolUISystem *ui;
    size_t       tab_index;
} TermTabClickCtx;

static TermTabClickCtx g_term_tab_ctxs[SOL_TERM_MAX_TABS];
static TermTabClickCtx g_term_tab_close_ctxs[SOL_TERM_MAX_TABS];

typedef struct TermViewportClickCtx {
    SolUISystem *ui;
} TermViewportClickCtx;

static TermViewportClickCtx g_term_viewport_ctx;

/*
 * Make the tab at tab_index the active one.
 * No-ops safely if the manager is empty or tab_index is stale (e.g. a
 * click resolving against a context slot from before the tab strip last
 * shrank) — without this guard, active_index can never reach an
 * out-of-range tab_index and the loop below would spin the UI thread
 * forever.
 *
 * mgr        Terminal manager owning the tabs.
 * tab_index  Target tab index to make active.
 */
static void term_tab_navigate_to(SolTerminalManager *mgr, size_t tab_index)
{
    size_t count = sol_terminal_manager_count(mgr);
    if (count == 0 || tab_index >= count) return;
    while (sol_terminal_manager_active_index(mgr) != tab_index) {
        if (sol_terminal_manager_active_index(mgr) < tab_index)
            sol_terminal_manager_next_tab(mgr);
        else
            sol_terminal_manager_prev_tab(mgr);
    }
}

static void on_term_tab_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    TermTabClickCtx *ctx = (TermTabClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->terminal_mgr) return;
    term_tab_navigate_to(ctx->ui->terminal_mgr, ctx->tab_index);
    sol_ui_system_terminal_set_focused(ctx->ui, true);
    sol_ui_system_terminal_notify(ctx->ui);
}

/*
 * Close the terminal tab at ctx->tab_index.
 * Switches active index to the target tab then closes it via the manager,
 * which handles PTY kill and memory cleanup internally.
 *
 * btn        Unused Ca_Button pointer.
 * user_data  TermTabClickCtx* identifying which tab to close.
 */
static void on_term_tab_close(Ca_Button *btn, void *user_data)
{
    (void)btn;
    TermTabClickCtx *ctx = (TermTabClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->terminal_mgr) return;
    SolTerminalManager *mgr = ctx->ui->terminal_mgr;
    if (ctx->tab_index >= sol_terminal_manager_count(mgr)) return;
    term_tab_navigate_to(mgr, ctx->tab_index);
    sol_terminal_manager_close_active(mgr);
    sol_ui_system_terminal_notify(ctx->ui);
}

/*
 * Open a terminal tab through the registered command path.
 *
 * btn        Unused Ca_Button pointer.
 * user_data  SolUISystem owning the terminal panel.
 */
static void on_term_tab_new(Ca_Button *btn, void *user_data)
{
    (void)btn;
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) return;
    (void)sol_ui_system_invoke_command(ui, "terminal.tab.new");
}

static void on_term_viewport_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    TermViewportClickCtx *ctx = (TermViewportClickCtx *)user_data;
    if (!ctx || !ctx->ui || !ctx->ui->terminal_mgr) return;
    SolTerminalManager *mgr = ctx->ui->terminal_mgr;
    if (!sol_terminal_manager_focused(mgr)) {
        sol_ui_system_terminal_set_focused(ctx->ui, true);
        sol_ui_system_terminal_notify(ctx->ui);
    }
}

/* ================================================================== */
/* Row rendering                                                       */
/* ================================================================== */

/* Share of the foreground alpha kept for SGR 2 (faint) text. */
#define TERM_DIM_ALPHA_NUM 3u
#define TERM_DIM_ALPHA_DEN 5u

#define TERM_STYLE_BASES        6u   /* 4 font variants + 2 cursor kinds */
#define TERM_STYLE_DECORATIONS  4u   /* none, underline, strike, both    */
#define TERM_STYLE_UL_STYLES    5u   /* SolTermUnderlineStyle values     */
#define TERM_STYLE_MAX_LEN      96u

/*
 * Pick the CSS class list for a cell: font variant (or cursor kind), SGR 4 /
 * SGR 9 decoration and the SGR 4:n underline style. Every combination is
 * built once into a fixed table, so equal styles share one pointer and run
 * boundaries can compare styles by pointer.
 *
 * attrs      Cell SOL_TERM_ATTR_* flags.
 * is_cursor  Whether the cursor sits on the cell.
 * focused    Whether the terminal panel has keyboard focus.
 * Returns    Space-separated class names (static storage).
 */
static const char *term_cell_style(uint16_t attrs, bool is_cursor, bool focused)
{
    static const char *const bases[TERM_STYLE_BASES] = {
        "term-cell", "term-cell-bold", "term-cell-italic", "term-cell-bold-italic",
        "term-cursor-unfocused", "term-cursor-focused",
    };
    static const char *const decorations[TERM_STYLE_DECORATIONS] = {
        "", " term-underline", " term-strike", " term-underline-strike",
    };
    static const char *const ul_styles[TERM_STYLE_UL_STYLES] = {
        "", " term-ul-double", " term-ul-curly", " term-ul-dotted", " term-ul-dashed",
    };
    static char table[TERM_STYLE_BASES][TERM_STYLE_DECORATIONS][TERM_STYLE_UL_STYLES]
                     [TERM_STYLE_MAX_LEN];
    static bool built = false;
    if (!built) {
        for (size_t b = 0; b < TERM_STYLE_BASES; ++b)
            for (size_t d = 0; d < TERM_STYLE_DECORATIONS; ++d)
                for (size_t u = 0; u < TERM_STYLE_UL_STYLES; ++u)
                    snprintf(table[b][d][u], TERM_STYLE_MAX_LEN, "%s%s%s",
                             bases[b], decorations[d], (d & 1u) ? ul_styles[u] : "");
        built = true;
    }

    const size_t base = is_cursor
        ? (focused ? 5u : 4u)
        : (((attrs & SOL_TERM_ATTR_BOLD)   ? 1u : 0u) |
           ((attrs & SOL_TERM_ATTR_ITALIC) ? 2u : 0u));
    const size_t decoration = ((attrs & SOL_TERM_ATTR_UNDERLINE) ? 1u : 0u) |
                              ((attrs & SOL_TERM_ATTR_STRIKE)    ? 2u : 0u);
    size_t ul_style = (decoration & 1u) ? (size_t)sol_term_underline_style(attrs) : 0u;
    if (ul_style >= TERM_STYLE_UL_STYLES) ul_style = 0u;
    return table[base][decoration][ul_style];
}

/*
 * Apply SGR 2 (faint) to a foreground colour by reducing its alpha, so the
 * text fades toward whatever panel background shows through it.
 *
 * rgba     Foreground colour as 0xRRGGBBAA.
 * Returns  The same colour at TERM_DIM_ALPHA_NUM/DEN of its alpha.
 */
static uint32_t term_dim_rgba(uint32_t rgba)
{
    const uint32_t alpha = (rgba & 0xFFu) * TERM_DIM_ALPHA_NUM / TERM_DIM_ALPHA_DEN;
    return (rgba & 0xFFFFFF00u) | alpha;
}

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
    uint32_t run_fg = 0u, run_bg = 0u, run_ul = 0u;
    const char *run_style = "term-cell";
    bool run_started = false;
    bool prev_cursor = false;

    for (int c = 0; c < cols; ++c) {
        const SolTermCell *cell     = &line->cells[c];
        const bool         is_cursor = (c == cursor_col);

        /* Trailing half of a double-width pair: the lead cell's glyph already
           spans this column, so emitting anything here would desync the row.
           The cursor still renders on it so it stays visible over wide text. */
        if ((cell->attrs & SOL_TERM_ATTR_WIDE_TAIL) && !is_cursor)
            continue;

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
            if (cell->attrs & SOL_TERM_ATTR_DIM) cell_fg = term_dim_rgba(cell_fg);
        }
        /* Explicit SGR 58 colour for the underline; 0 draws it in the text
           colour, as does a cell without underline. */
        const uint32_t cell_ul =
            ((cell->attrs & SOL_TERM_ATTR_UNDERLINE) && cell->ul.mode != SOL_TERM_COLOR_DEFAULT)
                ? sol_term_color_to_rgba(&cell->ul, true)
                : 0u;

        const char *cell_style = term_cell_style(cell->attrs, is_cursor, focused);

        /* Decide whether this cell continues the current run or starts a new one. */
        const bool new_run = !run_started ||
                             is_cursor != prev_cursor ||
                             cell_fg != run_fg ||
                             cell_bg != run_bg ||
                             cell_ul != run_ul ||
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
                .decoration_color = run_ul,
            });
            ca_div_end();
            run_len = 0;
        }

        if (new_run || !run_started) {
            run_fg      = cell_fg;
            run_bg      = cell_bg;
            run_ul      = cell_ul;
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
            .decoration_color = run_ul,
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
    ui->term_header_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "term-header workspace-panel-chrome",
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
            .skip_keyboard_focus = true,
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
            .skip_keyboard_focus = true,
        });
        ca_text(&(Ca_TextDesc){
            .text  = "\xC3\x97",   /* UTF-8 for U+00D7 MULTIPLICATION SIGN (×) */
            .style = active ? "term-tab-close-icon-active" : "term-tab-close-icon",
        });
        ca_btn_end();  /* term-tab-close */
        ca_btn_end();  /* term-tab / term-tab-active */
    }
    ca_btn_begin(&(Ca_BtnDesc){
        .style      = "term-tab-new",
        .direction  = CA_HORIZONTAL,
        .background = 0u,
        .on_click   = on_term_tab_new,
        .click_data = ui,
        .skip_keyboard_focus = true,
    });
    ca_text(&(Ca_TextDesc){
        .text  = CA_ICON_NF_FA_PLUS,
        .style = "term-tab-new-icon",
    });
    ca_btn_end();  /* term-tab-new */
    ca_div_end();   /* term-header */

    /* ---- Viewport (clickable to claim focus) ---- */
    g_term_viewport_ctx.ui = ui;
    ui->term_viewport_host = ca_btn_begin(&(Ca_BtnDesc){
        .style      = "term-viewport workspace-panel-well",
        .direction  = CA_VERTICAL,
        .background = 0u,
        .on_click   = on_term_viewport_click,
        .click_data = &g_term_viewport_ctx,
        /* All keyboard input while the terminal is focused goes straight to
           the PTY via input_router.c — this button must never enter
           Causality's own Tab-focus/Enter-activation cycle. Without this,
           clicking the viewport claims Causality's internal keyboard focus
           (win->focused_node), and every later Enter/Space keystroke
           re-fires on_term_viewport_click as a synthetic "activation" on
           top of being forwarded to the PTY, and bumps sig_terminal_rev a
           second time mid-frame. */
        .skip_keyboard_focus = true,
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
        .style     = "term-filler workspace-panel-well",
    });
    ca_div_end();

    ca_btn_end();   /* term-viewport */
}
