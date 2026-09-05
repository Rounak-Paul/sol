// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_terminal.c — regression coverage for the VT state machine's
 * scroll-region (DECSTBM) handling at degenerate terminal sizes.
 *
 * Whitebox: includes sol_terminal.c directly to reach its static VT
 * state machine (vt_process_byte) and construct a SolTerminal without
 * spawning a real PTY/SSH channel or Causality instance.
 */

#include "sol_terminal.c"

#include <stdio.h>
#include <string.h>

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

int main(void)
{
    test_decstbm_single_row_no_negative_margin();
    test_decstbm_normal_size_still_works();
    test_decstbm_zero_rows_is_inert();

    if (g_failures == 0) {
        printf("all terminal tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d terminal test failure(s)\n", g_failures);
    return 1;
}
