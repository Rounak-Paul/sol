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

int main(void)
{
    test_decstbm_single_row_no_negative_margin();
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
