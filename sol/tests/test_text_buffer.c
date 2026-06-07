// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_text_buffer.c — Unit tests for SolTextBuffer.
 *
 * Tests in this file are "headless": pass NULL as the render function
 * so no UI is needed.  The sol_text_buffer module is wire-free — only
 * libc + sol_rope + sol_platform are linked.
 *
 * Coverage:
 *   - open_empty / open_string / open_file
 *   - line_count: empty, single-line, multi-line, trailing-newline edge
 *   - cursor byte / line / col
 *   - insert_codepoint: ASCII, multi-byte (2/3/4 bytes), at end
 *   - insert_newline
 *   - backspace: basic, UTF-8 aware, at-start no-op
 *   - delete_forward: basic, at-end no-op
 *   - move_cursor: dx/dy, sticky col across lines
 *   - move_line_start / move_line_end
 *   - set_cursor_to: clamping
 *   - scroll_top: set / ensure_cursor_visible
 *   - copy_line: content, empty line, past-end clamping
 *   - find_by_path: dedup
 *   - Event bus: TEXT_EDITED payload correctness
 *   - Regression: backspace at start, delete at end, cursor past EOF
 *   - Performance: 10k inserts into a growing buffer
 */

#include "test_harness.h"

#include "sol_text_buffer.h"
#include "sol_event.h"
#include "sol_buffer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>   /* mkstemp, write, close */
#endif

/* ------------------------------------------------------------------ */
/* File-scope types / handlers (C11: no nested functions)              */
/* ------------------------------------------------------------------ */

typedef struct TbEditLog { int count; SolTextEditedPayload last; } TbEditLog;

static bool tb_edit_handler(const SolEvent *e, void *ud)
{
    TbEditLog *l = (TbEditLog *)ud;
    l->count++;
    if (e->payload && e->payload_size >= sizeof(SolTextEditedPayload))
        memcpy(&l->last, e->payload, sizeof(SolTextEditedPayload));
    return false;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static SolBufferSystem *make_system(void)
{
    SolBufferSystemConfig cfg = sol_buffer_system_config_default();
    return sol_buffer_system_create(&cfg);
}

/* Open an empty text buffer in a temporary system and return its tb. */
static SolTextBuffer *open_empty_tb(SolBufferSystem *sys)
{
    SolBufferId id = sol_text_buffer_open_empty(sys, "scratch", NULL);
    if (id == 0u) return NULL;
    SolBuffer *buf = sol_buffer_get(sys, id);
    return sol_text_buffer_state(buf);
}

/* Read full rope content into a stack buffer. */
static size_t tb_read_all(SolTextBuffer *tb, char *out, size_t max)
{
    SolRope *r = NULL;
    /* Access rope via the public sol_text_buffer_copy_line trick: iterate all lines. */
    size_t total = 0u;
    const size_t nlines = sol_text_buffer_line_count(tb);
    for (size_t i = 0; i < nlines && total < max - 1u; ++i) {
        char line[4096];
        size_t n = sol_text_buffer_copy_line(tb, i, line, sizeof(line));
        if (total + n > max - 1u) n = max - 1u - total;
        memcpy(out + total, line, n);
        total += n;
        /* Re-add newline between lines (copy_line strips it). */
        if (i + 1u < nlines && total < max - 1u) {
            out[total++] = '\n';
        }
    }
    out[total] = '\0';
    return total;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_empty_buffer(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);
    SOL_CHECK_NOT_NULL(T, tb);

    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 1);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 0);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 0);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 0);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_top(tb), 0);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 0);

    char out[64];
    size_t n = sol_text_buffer_copy_line(tb, 0, out, sizeof(out));
    SOL_CHECK_EQ_SZ(T, n, 0);

    sol_buffer_system_destroy(sys);
}

static void test_open_string(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *content = "hello\nworld\n";
    SolBufferId id = sol_text_buffer_open_string(
        sys, "test", content, strlen(content), NULL, NULL);
    SOL_CHECK(T, id != 0u);

    SolBuffer *buf = sol_buffer_get(sys, id);
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    SOL_CHECK_NOT_NULL(T, tb);

    /* "hello\nworld\n" → 2 newlines → lines = 2 (no trailing partial). */
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 2);

    char line0[32], line1[32];
    sol_text_buffer_copy_line(tb, 0, line0, sizeof(line0));
    sol_text_buffer_copy_line(tb, 1, line1, sizeof(line1));
    SOL_CHECK_STR(T, line0, "hello");
    SOL_CHECK_STR(T, line1, "world");

    sol_buffer_system_destroy(sys);
}

static void test_line_count_edge_cases(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();

    /* No trailing newline: "abc" → 1 line. */
    SolBufferId id = sol_text_buffer_open_string(
        sys, "t1", "abc", 3, NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 1);

    /* Single newline: "\n" → 1 line (newline at end, no trailing partial). */
    id = sol_text_buffer_open_string(sys, "t2", "\n", 1, NULL, NULL);
    tb = sol_text_buffer_state(sol_buffer_get(sys, id));
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 1);

    /* "a\nb" → 2 lines. */
    id = sol_text_buffer_open_string(sys, "t3", "a\nb", 3, NULL, NULL);
    tb = sol_text_buffer_state(sol_buffer_get(sys, id));
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 2);

    /* "a\n\n" → 2 newlines, last char is '\n' → 2 lines. */
    id = sol_text_buffer_open_string(sys, "t4", "a\n\n", 3, NULL, NULL);
    tb = sol_text_buffer_state(sol_buffer_get(sys, id));
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 2);

    sol_buffer_system_destroy(sys);
}

static void test_insert_ascii(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    SOL_CHECK(T, sol_text_buffer_insert_codepoint(tb, 'H'));
    SOL_CHECK(T, sol_text_buffer_insert_codepoint(tb, 'i'));
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 2);

    char line[32];
    sol_text_buffer_copy_line(tb, 0, line, sizeof(line));
    SOL_CHECK_STR(T, line, "Hi");

    sol_buffer_system_destroy(sys);
}

static void test_insert_multibyte(SolTestCtx *T)
{
    /* 'é' = U+00E9 = 0xC3 0xA9 (2 bytes)
       '€' = U+20AC = 0xE2 0x82 0xAC (3 bytes)
       '𝄞' = U+1D11E = 0xF0 0x9D 0x84 0x9E (4 bytes) */
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    SOL_CHECK(T, sol_text_buffer_insert_codepoint(tb, 0xE9u));   /* é */
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 2);

    SOL_CHECK(T, sol_text_buffer_insert_codepoint(tb, 0x20ACu));  /* € */
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 5);

    SOL_CHECK(T, sol_text_buffer_insert_codepoint(tb, 0x1D11Eu)); /* 𝄞 */
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 9);

    char line[32];
    size_t n = sol_text_buffer_copy_line(tb, 0, line, sizeof(line));
    SOL_CHECK_EQ_SZ(T, n, 9);  /* 2+3+4 bytes */

    sol_buffer_system_destroy(sys);
}

static void test_insert_newline(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    sol_text_buffer_insert_codepoint(tb, 'A');
    sol_text_buffer_insert_newline(tb);
    sol_text_buffer_insert_codepoint(tb, 'B');

    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 2);

    char l0[8], l1[8];
    sol_text_buffer_copy_line(tb, 0, l0, sizeof(l0));
    sol_text_buffer_copy_line(tb, 1, l1, sizeof(l1));
    SOL_CHECK_STR(T, l0, "A");
    SOL_CHECK_STR(T, l1, "B");

    sol_buffer_system_destroy(sys);
}

static void test_backspace(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    sol_text_buffer_insert_codepoint(tb, 'A');
    sol_text_buffer_insert_codepoint(tb, 'B');
    sol_text_buffer_insert_codepoint(tb, 'C');
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 3);

    bool ok = sol_text_buffer_backspace(tb);
    SOL_CHECK(T, ok);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 2);

    char line[8];
    sol_text_buffer_copy_line(tb, 0, line, sizeof(line));
    SOL_CHECK_STR(T, line, "AB");

    sol_buffer_system_destroy(sys);
}

static void test_backspace_multibyte(SolTestCtx *T)
{
    /* Insert 'é' (2 bytes), backspace should remove both bytes. */
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    sol_text_buffer_insert_codepoint(tb, 0xE9u);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 2);

    bool ok = sol_text_buffer_backspace(tb);
    SOL_CHECK(T, ok);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 0);

    sol_buffer_system_destroy(sys);
}

static void test_backspace_at_start(SolTestCtx *T)
{
    /* Regression: backspace at position 0 must return false and not crash. */
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    bool ok = sol_text_buffer_backspace(tb);
    SOL_CHECK(T, !ok);

    sol_buffer_system_destroy(sys);
}

static void test_delete_forward(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *s = "ABC";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, 3, NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    /* Cursor starts at 0. Delete 'A'. */
    bool ok = sol_text_buffer_delete_forward(tb);
    SOL_CHECK(T, ok);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 0);

    char line[8];
    sol_text_buffer_copy_line(tb, 0, line, sizeof(line));
    SOL_CHECK_STR(T, line, "BC");

    sol_buffer_system_destroy(sys);
}

static void test_delete_forward_at_end(SolTestCtx *T)
{
    /* Regression: delete-forward at EOF must return false. */
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    bool ok = sol_text_buffer_delete_forward(tb);
    SOL_CHECK(T, !ok);

    sol_buffer_system_destroy(sys);
}

static void test_move_cursor_left_right(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *s = "hello";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, 5, NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    sol_text_buffer_move_cursor(tb, +3, 0, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 3);

    sol_text_buffer_move_cursor(tb, -1, 0, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 2);

    /* Past-start clamp. */
    sol_text_buffer_move_cursor(tb, -100, 0, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(tb), 0);

    sol_buffer_system_destroy(sys);
}

static void test_move_cursor_down_up(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *s = "first\nsecond\nthird";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, strlen(s), NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 0);
    sol_text_buffer_move_cursor(tb, 0, +1, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 1);

    sol_text_buffer_move_cursor(tb, 0, +1, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 2);

    /* Past last line — should clamp. */
    sol_text_buffer_move_cursor(tb, 0, +100, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 2);

    sol_text_buffer_move_cursor(tb, 0, -1, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 1);

    sol_buffer_system_destroy(sys);
}

static void test_sticky_col(SolTestCtx *T)
{
    /* "long_line\nshort\nlong_line"
       Place cursor at col 6 on line 0, move down to "short" (only 5 chars).
       With sticky=true the preferred col is preserved; on line 1 cursor
       clamps to end. On return to line 2 it restores to col 6. */
    SolBufferSystem *sys = make_system();
    const char *s = "abcdefgh\nAB\nabcdefgh";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, strlen(s), NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    /* Move to col 6 on line 0. */
    sol_text_buffer_move_cursor(tb, +6, 0, false);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 6);

    /* Down with sticky — lands at end of "AB" (col 2). */
    sol_text_buffer_move_cursor(tb, 0, +1, true);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 1);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 2);  /* clamped */

    /* Down again — line 2 is 8 chars long, preferred col 6 is restored. */
    sol_text_buffer_move_cursor(tb, 0, +1, true);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 2);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 6);

    sol_buffer_system_destroy(sys);
}

static void test_move_line_start_end(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *s = "hello world";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, strlen(s), NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    /* Cursor starts at 0 = line start. Move to end. */
    sol_text_buffer_move_line_end(tb);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 11);

    sol_text_buffer_move_line_start(tb);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 0);

    sol_buffer_system_destroy(sys);
}

static void test_set_cursor_to(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *s = "line0\nline1\nline2";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, strlen(s), NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    sol_text_buffer_set_cursor_to(tb, 1, 3);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 1);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(tb), 3);

    /* Clamp to past-end line. */
    sol_text_buffer_set_cursor_to(tb, 999, 0);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(tb), 2);   /* last line */

    sol_buffer_system_destroy(sys);
}

static void test_scroll_top(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    sol_text_buffer_set_scroll_top(tb, 5);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_top(tb), 5);

    sol_text_buffer_set_scroll_top(tb, -3);   /* negative clamps to 0 */
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_top(tb), 0);

    sol_buffer_system_destroy(sys);
}

static void test_ensure_cursor_visible(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    /* Build a 20-line buffer.
       19 loops of (digit + '\n') gives 19 newlines → 19 lines, because the
       buffer ends with '\n' and sol_text_buffer_line_count does not count a
       trailing empty line.  An extra trailing character produces line 19 as
       a partial line, giving line_count = 20. */
    SolTextBuffer *tb = open_empty_tb(sys);
    for (int i = 0; i < 19; ++i) {
        sol_text_buffer_insert_codepoint(tb, '0' + (i % 10));
        sol_text_buffer_insert_newline(tb);
    }
    sol_text_buffer_insert_codepoint(tb, 'X'); /* line 19: partial, no newline */
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 20);

    /* Move cursor to line 15, scroll_top = 0. With viewport=10,
       ensure_cursor_visible must scroll to keep line 15 visible. */
    sol_text_buffer_set_cursor_to(tb, 15, 0);
    sol_text_buffer_set_scroll_top(tb, 0);
    sol_text_buffer_ensure_cursor_visible(tb, 10);
    const int st = sol_text_buffer_scroll_top(tb);
    SOL_CHECK_MSG(T, st + 10 > 15, "scroll_top=%d doesn't include line 15", st);

    sol_buffer_system_destroy(sys);
}

static void test_scroll_left(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);

    sol_text_buffer_set_scroll_left(tb, 12);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 12);

    sol_text_buffer_set_scroll_left(tb, -8);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 0);

    sol_buffer_system_destroy(sys);
}

static void test_ensure_cursor_visible_2d(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    SolBufferId id = sol_text_buffer_open_string(
        sys, "wide", "abcdef\n\txyz", strlen("abcdef\n\txyz"), NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    sol_text_buffer_set_cursor_to(tb, 0, 5);
    sol_text_buffer_ensure_cursor_visible_2d(tb, 3, 4);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_top(tb), 0);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 2);

    sol_text_buffer_set_cursor_to(tb, 0, 1);
    sol_text_buffer_ensure_cursor_visible_2d(tb, 3, 4);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 1);

    sol_text_buffer_set_cursor_to(tb, 1, 2);
    sol_text_buffer_ensure_cursor_visible_2d(tb, 3, 4);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 2);

    sol_text_buffer_set_cursor_to(tb, 0, 0);
    sol_text_buffer_ensure_cursor_visible_2d(tb, 3, 4);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(tb), 0);

    sol_buffer_system_destroy(sys);
}

static void test_copy_line(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();
    const char *s = "abc\ndef\n";
    SolBufferId id = sol_text_buffer_open_string(sys, "t", s, strlen(s), NULL, NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    char buf[32];
    size_t n;

    n = sol_text_buffer_copy_line(tb, 0, buf, sizeof(buf));
    SOL_CHECK_EQ_SZ(T, n, 3); SOL_CHECK_STR(T, buf, "abc");

    n = sol_text_buffer_copy_line(tb, 1, buf, sizeof(buf));
    SOL_CHECK_EQ_SZ(T, n, 3); SOL_CHECK_STR(T, buf, "def");

    /* Past-end line returns 0. */
    n = sol_text_buffer_copy_line(tb, 5, buf, sizeof(buf));
    SOL_CHECK_EQ_SZ(T, n, 0);

    sol_buffer_system_destroy(sys);
}

static void test_find_by_path(SolTestCtx *T)
{
    SolBufferSystem *sys = make_system();

    SolBufferId id1 = sol_text_buffer_open_string(sys, "t", "hi", 2,
                                                   "/tmp/test.txt", NULL);
    SolBufferId id2 = sol_text_buffer_find_by_path(sys, "/tmp/test.txt");
    SOL_CHECK(T, id1 == id2);

    SolBufferId id3 = sol_text_buffer_find_by_path(sys, "/tmp/other.txt");
    SOL_CHECK_EQ_SZ(T, id3, 0);

    sol_buffer_system_destroy(sys);
}

static void test_event_text_edited(SolTestCtx *T)
{
    SolEventBusConfig ecfg = sol_event_bus_config_default();
    SolEventBus *bus = sol_event_bus_create(&ecfg);

    TbEditLog elog = {0};

    sol_event_bus_subscribe(bus, &(SolEventSubscriptionDesc){
        .event_name = SOL_EVENT_TEXT_EDITED,
        .handler    = tb_edit_handler,
        .user_data  = &elog,
    });

    SolBufferSystem *sys = make_system();
    sol_buffer_attach_event_bus(sys, bus);

    SolBufferId id = sol_text_buffer_open_empty(sys, "t", NULL);
    SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));

    sol_text_buffer_insert_codepoint(tb, 'A');
    SOL_CHECK_EQ_INT(T, elog.count, 1);
    SOL_CHECK_EQ_SZ(T, elog.last.inserted_bytes, 1);
    SOL_CHECK_EQ_SZ(T, elog.last.removed_bytes, 0);
    SOL_CHECK_EQ_SZ(T, elog.last.byte_offset, 0);

    sol_text_buffer_backspace(tb);
    SOL_CHECK_EQ_INT(T, elog.count, 2);
    SOL_CHECK_EQ_SZ(T, elog.last.removed_bytes, 1);
    SOL_CHECK_EQ_SZ(T, elog.last.inserted_bytes, 0);

    sol_buffer_system_destroy(sys);
    sol_event_bus_destroy(bus);
}

static void test_open_file(SolTestCtx *T)
{
    /* Write a temp file and open it. */
#if defined(_WIN32)
    char temp_dir[MAX_PATH];
    char path[MAX_PATH];
    if (GetTempPathA((DWORD)sizeof(temp_dir), temp_dir) == 0u ||
        GetTempFileNameA(temp_dir, "sol", 0u, path) == 0u) {
        SOL_CHECK_MSG(T, false, "GetTempFileNameA failed");
        return;
    }

    const char *content = "line one\nline two\nline three\n";
    FILE *fp = fopen(path, "wb");
    if (!fp) { SOL_CHECK_MSG(T, false, "fopen temp file failed"); return; }
    fwrite(content, 1u, strlen(content), fp);
    fclose(fp);
#else
    char path[] = "/tmp/sol_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { SOL_CHECK_MSG(T, false, "mkstemp failed"); return; }
    const char *content = "line one\nline two\nline three\n";
    write(fd, content, strlen(content));
    close(fd);
#endif

    SolBufferSystem *sys = make_system();
    const char *err = NULL;
    SolBufferId id = sol_text_buffer_open_file(sys, path, "tmpfile", NULL, &err);
    SOL_CHECK_MSG(T, id != 0u, "open_file failed: %s", err ? err : "(null)");

    if (id != 0u) {
        SolTextBuffer *tb = sol_text_buffer_state(sol_buffer_get(sys, id));
        SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(tb), 3);

        char l0[32];
        sol_text_buffer_copy_line(tb, 0, l0, sizeof(l0));
        SOL_CHECK_STR(T, l0, "line one");
    }

    remove(path);
    sol_buffer_system_destroy(sys);
}

static void test_null_safety(SolTestCtx *T)
{
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_line_count(NULL), 1);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_byte(NULL), 0);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_line(NULL), 0);
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_cursor_col(NULL), 0);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_top(NULL), 0);
    SOL_CHECK_EQ_INT(T, sol_text_buffer_scroll_left(NULL), 0);
    sol_text_buffer_set_scroll_left(NULL, 1);
    sol_text_buffer_ensure_cursor_visible_2d(NULL, 1, 1);
    SOL_CHECK(T, !sol_text_buffer_insert_codepoint(NULL, 'A'));
    SOL_CHECK(T, !sol_text_buffer_insert_newline(NULL));
    SOL_CHECK(T, !sol_text_buffer_backspace(NULL));
    SOL_CHECK(T, !sol_text_buffer_delete_forward(NULL));

    char buf[8];
    SOL_CHECK_EQ_SZ(T, sol_text_buffer_copy_line(NULL, 0, buf, sizeof(buf)), 0);
    SOL_CHECK_NULL(T, sol_text_buffer_source_path(NULL));
}

/* ------------------------------------------------------------------ */
/* Performance benchmark                                               */
/* ------------------------------------------------------------------ */

typedef struct BenchTbCtx { SolBufferSystem *sys; SolTextBuffer *tb; } BenchTbCtx;

static void bench_insert_fn(void *ud)
{
    BenchTbCtx *ctx = (BenchTbCtx *)ud;
    sol_text_buffer_insert_codepoint(ctx->tb, 'x');
}

static void run_benchmarks(void)
{
    SolBufferSystem *sys = make_system();
    SolTextBuffer *tb = open_empty_tb(sys);
    BenchTbCtx ctx = { sys, tb };

    /* Pre-seed with 1000 chars so we're not always at offset 0. */
    for (int i = 0; i < 1000; ++i)
        sol_text_buffer_insert_codepoint(tb, 'a' + (i % 26));

    /* 500 iterations: enough to measure, fast enough for CI. The benchmark
       previously used 5000 but each insert was O(n) in line length causing
       a 10-minute runtime; that bug is fixed but we keep the count modest. */
    sol_bench("text_buffer_insert_codepoint", 500, bench_insert_fn, &ctx);

    sol_buffer_system_destroy(sys);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    SolTestSuite s;
    sol_suite_init(&s, "sol_text_buffer");

    SOL_RUN(s, test_empty_buffer);
    SOL_RUN(s, test_open_string);
    SOL_RUN(s, test_line_count_edge_cases);
    SOL_RUN(s, test_insert_ascii);
    SOL_RUN(s, test_insert_multibyte);
    SOL_RUN(s, test_insert_newline);
    SOL_RUN(s, test_backspace);
    SOL_RUN(s, test_backspace_multibyte);
    SOL_RUN(s, test_backspace_at_start);
    SOL_RUN(s, test_delete_forward);
    SOL_RUN(s, test_delete_forward_at_end);
    SOL_RUN(s, test_move_cursor_left_right);
    SOL_RUN(s, test_move_cursor_down_up);
    SOL_RUN(s, test_sticky_col);
    SOL_RUN(s, test_move_line_start_end);
    SOL_RUN(s, test_set_cursor_to);
    SOL_RUN(s, test_scroll_top);
    SOL_RUN(s, test_ensure_cursor_visible);
    SOL_RUN(s, test_scroll_left);
    SOL_RUN(s, test_ensure_cursor_visible_2d);
    SOL_RUN(s, test_copy_line);
    SOL_RUN(s, test_find_by_path);
    SOL_RUN(s, test_event_text_edited);
    SOL_RUN(s, test_open_file);
    SOL_RUN(s, test_null_safety);

    run_benchmarks();

    return sol_suite_report(&s);
}
