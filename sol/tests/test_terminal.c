// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_terminal.c — regression coverage for the VT state machine's
 * scroll-region (DECSTBM) handling at degenerate terminal sizes.
 *
 * Whitebox: includes sol_terminal.c directly to reach its static VT
 * state machine (vt_process_byte) and construct a SolTerminal without
 * spawning a real PTY/SSH channel or Causality instance.
 */

#define ca_instance_wake sol_terminal_test_wake
#include "sol_terminal.c"
#undef ca_instance_wake

#include <stdio.h>
#include <string.h>

/* Isolate PTY tests from Causality's process-global GLFW event loop. */
void sol_terminal_test_wake(void) {}

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void feed(SolTerminal *term, const char *seq)
{
    for (const char *p = seq; *p; ++p) vt_process_byte(term, (uint8_t)*p);
}

/*
 * DECSTBM (set scroll region) at rows == 1 used to compute
 * margin_top = term_clamp(v, 0, rows - 2) with rows - 2 == -1, an
 * inverted [0, -1] clamp range that returns -1. The next linefeed's
 * scroll-up loop then indexed term->screen[-1] — one SolTermLine
 * before the array, corrupting adjacent SolTerminal fields.
 *
 * Repro: shrink the terminal to 1 row, then feed a scroll-region
 * escape sequence (CSI r) followed by any newline — exactly what a
 * TUI that sets a scroll region on every redraw would send.
 */
static void test_decstbm_single_row_no_negative_margin(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));

    sol_terminal_resize(&term, 80, 24);
    CHECK(term.rows == 24 && term.cols == 80);

    sol_terminal_resize(&term, 80, 1);
    CHECK(term.rows == 1);

    feed(&term, "\x1b[1;1r\n");
    CHECK(term.margin_top >= 0);
    CHECK(term.margin_bottom >= term.margin_top);

    /* Repeat cycles to catch anything that only manifests after
       several scroll-region resets at this degenerate size. */
    for (int i = 0; i < 100; ++i) {
        feed(&term, "\x1b[1;1r\n\x1b[?25h");
        CHECK(term.margin_top >= 0);
    }
}

/* Functional regression check: DECSTBM must still set a normal scroll
 * region correctly at an ordinary terminal size. */
static void test_decstbm_normal_size_still_works(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));

    sol_terminal_resize(&term, 80, 24);
    feed(&term, "\x1b[5;20r\n");
    CHECK(term.margin_top == 4);
    CHECK(term.margin_bottom == 19);
}

/* DECSTBM at rows == 0 must also be inert (defensive: sol_terminal_resize
 * itself rejects rows < 1, but the CSI handler's own guard should not
 * rely solely on that). */
static void test_decstbm_zero_rows_is_inert(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    /* term.rows stays 0 (the zero-initialized state); resize is never
       called, so this exercises the CSI handler guard directly against
       whatever an uninitialized-but-zeroed terminal reports. */
    feed(&term, "\x1b[1;1r");
    CHECK(term.margin_top >= 0);
}

#if !defined(_WIN32)
/* An idle PTY reader must not make terminal destruction wait for a blocked
 * read. The command remains in the terminal foreground group when close is
 * requested, exercising the same teardown boundary as an interactive CLI. */
static void test_pty_close_is_bounded_with_foreground_command(void)
{
    setenv("SHELL", "/bin/sh", 1);
    SolTerminalManager *mgr = sol_terminal_manager_create((Ca_Instance *)(uintptr_t)1);
    CHECK(mgr != NULL);
    if (!mgr) return;

    SolTerminal *term = sol_terminal_manager_new_tab(mgr, NULL);
    CHECK(term != NULL);
    if (!term) {
        sol_terminal_manager_destroy(mgr);
        return;
    }

    sol_terminal_send_text(term, "sleep 30\n", 9u);
    usleep(20000);
    uint64_t start = sol_platform_now_monotonic_ns();
    sol_terminal_manager_close_active(mgr);
    uint64_t elapsed = sol_platform_now_monotonic_ns() - start;
    CHECK(elapsed < 750000000ull);
    sol_terminal_manager_destroy(mgr);
}
#endif

/* Feed UTF-8 text through the same decoder the PTY drain path uses. */
static void feed_utf8(SolTerminal *term, const char *text)
{
    for (const char *p = text; *p; ++p)
        vt_utf8_feed(term, &term->utf8_state, (uint8_t)*p);
}

/*
 * Cell widths follow ca_codepoint_cell_width: Claude Code's narrow status
 * glyphs (⏺ ⚠ ⏵) take one cell, emoji take two, and zero-width codepoints
 * (VS16, combining marks, ZWJ) take none — any other count shifts every
 * following column relative to the application's own layout.
 */
static void test_codepoint_cell_widths(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    sol_terminal_resize(&term, 40, 3);

    feed_utf8(&term, "\xE2\x8F\xBA\xE2\x9A\xA0\xEF\xB8\x8F"   /* ⏺ ⚠ VS16    */
                     "e\xCC\x81\xE2\x80\x8D"                 /* e + acute, ZWJ */
                     "\xF0\x9F\x99\x82"                       /* 🙂 (wide)      */
                     "\xE2\x8F\xB5|");                        /* ⏵ then marker  */
    const SolTermCell *cells = term.screen[0].cells;
    CHECK(cells[0].codepoint == 0x23FAu && !(cells[0].attrs & SOL_TERM_ATTR_WIDE));
    CHECK(cells[1].codepoint == 0x26A0u);
    CHECK(cells[2].codepoint == 'e');
    CHECK(cells[3].codepoint == 0x1F642u && (cells[3].attrs & SOL_TERM_ATTR_WIDE));
    CHECK(cells[4].attrs & SOL_TERM_ATTR_WIDE_TAIL);
    CHECK(cells[5].codepoint == 0x23F5u);
    CHECK(cells[6].codepoint == '|');
    CHECK(term.cur_col == 7);
}

/* SGR 2 marks cells faint until SGR 22, as Claude Code's dim prompt
   suggestions rely on. */
static void test_sgr_dim(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    sol_terminal_resize(&term, 10, 2);
    feed(&term, "\x1b[2mab\x1b[22mc");
    CHECK(term.screen[0].cells[0].attrs & SOL_TERM_ATTR_DIM);
    CHECK(term.screen[0].cells[1].attrs & SOL_TERM_ATTR_DIM);
    CHECK(!(term.screen[0].cells[2].attrs & SOL_TERM_ATTR_DIM));
}

/*
 * CSI > Pp m (XTMODKEYS) and CSI ? Pp m (XTQMODKEYS) are not SGR. Claude
 * Code sends CSI > 4 m on startup; read as SGR 4 it underlined all
 * following text.
 */
static void test_marked_csi_m_is_not_sgr(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    sol_terminal_resize(&term, 10, 2);
    feed(&term, "\x1b[>4m\x1b[>4;2m\x1b[?4mab\x1b[1mc");
    CHECK(term.screen[0].cells[0].attrs == 0u);
    CHECK(term.screen[0].cells[1].attrs == 0u);
    CHECK(term.screen[0].cells[2].attrs == SOL_TERM_ATTR_BOLD);
}

/*
 * ':' sub-parameters (ITU T.416) attach to the preceding SGR parameter:
 * 4:3 is curly underline — not "43", which is a yellow background — and
 * 4:0 turns underline off. Colon and semicolon colour forms both decode.
 */
static void test_sgr_subparameters(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    sol_terminal_resize(&term, 10, 2);

    feed(&term, "\x1b[4:3;9mA");
    const SolTermCell *a = &term.screen[0].cells[0];
    CHECK(a->attrs & SOL_TERM_ATTR_UNDERLINE);
    CHECK(a->attrs & SOL_TERM_ATTR_STRIKE);
    CHECK(a->bg.mode == SOL_TERM_COLOR_DEFAULT);

    feed(&term, "\x1b[4:0;29;38:2::10:20:300mB");
    const SolTermCell *b = &term.screen[0].cells[1];
    CHECK(!(b->attrs & (SOL_TERM_ATTR_UNDERLINE | SOL_TERM_ATTR_STRIKE)));
    CHECK(b->fg.mode == SOL_TERM_COLOR_RGB);
    CHECK(b->fg.rgb.r == 10 && b->fg.rgb.g == 20 && b->fg.rgb.b == 255);

    feed(&term, "\x1b[0;48;2;1;2;3;58:5:196;21mC");
    const SolTermCell *c = &term.screen[0].cells[2];
    CHECK(c->bg.mode == SOL_TERM_COLOR_RGB && c->bg.rgb.b == 3);
    CHECK(c->attrs & SOL_TERM_ATTR_UNDERLINE);
    CHECK(!(c->attrs & SOL_TERM_ATTR_BLINK));

    /* A leading ';' leaves the first parameter at its default. */
    feed(&term, "\x1b[;5H");
    CHECK(term.cur_row == 0 && term.cur_col == 4);

    /* Oversized numbers saturate instead of overflowing. */
    feed(&term, "\x1b[99999999999999999999C");
    CHECK(term.cur_col == term.cols - 1);
}

/* SGR 4:n selects the underline style; 4, 21 and 24 set/reset it. */
static void test_sgr_underline_styles(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    sol_terminal_resize(&term, 10, 2);
    feed(&term, "\x1b[4:3mA\x1b[4:4mB\x1b[21mC\x1b[4mD\x1b[4:5mE\x1b[24mF\x1b[4:9mG");
    const SolTermCell *c = term.screen[0].cells;
    CHECK((c[0].attrs & SOL_TERM_ATTR_UNDERLINE) && sol_term_underline_style(c[0].attrs) == SOL_TERM_UNDERLINE_CURLY);
    CHECK(sol_term_underline_style(c[1].attrs) == SOL_TERM_UNDERLINE_DOTTED);
    CHECK(sol_term_underline_style(c[2].attrs) == SOL_TERM_UNDERLINE_DOUBLE);
    CHECK(sol_term_underline_style(c[3].attrs) == SOL_TERM_UNDERLINE_SINGLE);
    CHECK(sol_term_underline_style(c[4].attrs) == SOL_TERM_UNDERLINE_DASHED);
    CHECK(!(c[5].attrs & (SOL_TERM_ATTR_UNDERLINE | SOL_TERM_ATTR_UL_MASK)));
    CHECK((c[6].attrs & SOL_TERM_ATTR_UNDERLINE) && sol_term_underline_style(c[6].attrs) == SOL_TERM_UNDERLINE_SINGLE);
}

/* SGR 58 sets the underline colour in both encodings; 59 and 0 reset it. */
static void test_sgr_underline_color(void)
{
    SolTerminal term;
    memset(&term, 0, sizeof(term));
    sol_terminal_resize(&term, 10, 2);
    feed(&term, "\x1b[4:3;58:2::255:0:0mA\x1b[58;5;196mB\x1b[59mC\x1b[58;2;1;2;3m\x1b[0mD");
    const SolTermCell *c = term.screen[0].cells;
    CHECK(c[0].ul.mode == SOL_TERM_COLOR_RGB && c[0].ul.rgb.r == 255 && c[0].ul.rgb.g == 0);
    CHECK(c[0].fg.mode == SOL_TERM_COLOR_DEFAULT);
    CHECK(c[1].ul.mode == SOL_TERM_COLOR_INDEXED && c[1].ul.index == 196);
    CHECK(c[2].ul.mode == SOL_TERM_COLOR_DEFAULT);
    CHECK(c[3].ul.mode == SOL_TERM_COLOR_DEFAULT && !(c[3].attrs & SOL_TERM_ATTR_UNDERLINE));
    /* Three colours per cell must not grow the grid past the old 24 bytes. */
    CHECK(sizeof(SolTermColor) == 4u);
    CHECK(sizeof(SolTermCell) <= 20u);
}

int main(void)
{
    test_sgr_underline_color();
    test_sgr_underline_styles();
    test_sgr_subparameters();
    test_decstbm_single_row_no_negative_margin();
    test_codepoint_cell_widths();
    test_sgr_dim();
    test_marked_csi_m_is_not_sgr();
    test_decstbm_normal_size_still_works();
    test_decstbm_zero_rows_is_inert();
#if !defined(_WIN32)
    test_pty_close_is_bounded_with_foreground_command();
#endif

    if (g_failures == 0) {
        printf("all terminal tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d terminal test failure(s)\n", g_failures);
    return 1;
}
