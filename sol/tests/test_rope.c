// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_rope.c — Unit tests for sol_rope.
 *
 * Tiny home-grown harness; no external dependency. Returns nonzero
 * exit code on the first failed assertion. */

#include "sol_rope.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond) do {                                          \
    if (!(cond)) {                                                \
        fprintf(stderr, "FAIL %s:%d: %s\n",                       \
                __FILE__, __LINE__, #cond);                       \
        g_failures++;                                             \
    }                                                             \
} while (0)

#define CHECK_EQ_SZ(a, b) do {                                    \
    size_t _a = (size_t)(a), _b = (size_t)(b);                    \
    if (_a != _b) {                                               \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%zu vs %zu)\n",    \
                __FILE__, __LINE__, #a, #b, _a, _b);              \
        g_failures++;                                             \
    }                                                             \
} while (0)

/* Read the entire rope into a freshly-allocated buffer. Caller frees. */
static uint8_t *rope_to_buffer(const SolRope *r, size_t *out_len)
{
    size_t n = sol_rope_byte_len(r);
    uint8_t *buf = (uint8_t *)malloc(n + 1u);
    assert(buf);
    size_t got = 0;
    while (got < n) {
        size_t k = sol_rope_read(r, got, buf + got, n - got);
        if (k == 0) break;
        got += k;
    }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static void test_empty(void)
{
    SolRope *r = sol_rope_create();
    CHECK(r != NULL);
    CHECK_EQ_SZ(sol_rope_byte_len(r), 0);
    CHECK_EQ_SZ(sol_rope_char_len(r), 0);
    CHECK_EQ_SZ(sol_rope_line_count(r), 0);
    sol_rope_destroy(r);
}

static void test_from_bytes_metrics(void)
{
    /* "héllo\nworld\n" : 12 bytes, 12 chars (é = 2 bytes, 1 char), 2 lines. */
    const char *s = "h\xc3\xa9llo\nworld\n";
    SolRope *r = sol_rope_from_bytes((const uint8_t *)s, strlen(s));
    CHECK(r != NULL);
    CHECK_EQ_SZ(sol_rope_byte_len(r), strlen(s));
    CHECK_EQ_SZ(sol_rope_char_len(r), 12);
    CHECK_EQ_SZ(sol_rope_line_count(r), 2);

    size_t got;
    uint8_t *buf = rope_to_buffer(r, &got);
    CHECK_EQ_SZ(got, strlen(s));
    CHECK(memcmp(buf, s, got) == 0);
    free(buf);
    sol_rope_destroy(r);
}

static void test_insert(void)
{
    SolRope *r = sol_rope_from_bytes((const uint8_t *)"hello world", 11);
    CHECK(sol_rope_insert(r, 5, (const uint8_t *)" cruel", 6));
    /* expect: "hello cruel world" */
    size_t got;
    uint8_t *buf = rope_to_buffer(r, &got);
    CHECK_EQ_SZ(got, 17);
    CHECK(memcmp(buf, "hello cruel world", 17) == 0);
    free(buf);

    /* insert at the very start and end */
    CHECK(sol_rope_insert(r, 0, (const uint8_t *)">> ", 3));
    CHECK(sol_rope_insert(r, sol_rope_byte_len(r), (const uint8_t *)" <<", 3));
    buf = rope_to_buffer(r, &got);
    CHECK_EQ_SZ(got, 23);
    CHECK(memcmp(buf, ">> hello cruel world <<", 23) == 0);
    free(buf);

    sol_rope_destroy(r);
}

static void test_remove(void)
{
    SolRope *r = sol_rope_from_bytes((const uint8_t *)"the quick brown fox", 19);
    CHECK(sol_rope_remove(r, 4, 6));   /* drop "quick " -> "the brown fox" */
    size_t got;
    uint8_t *buf = rope_to_buffer(r, &got);
    CHECK_EQ_SZ(got, 13);
    CHECK(memcmp(buf, "the brown fox", 13) == 0);
    free(buf);

    /* delete-all-and-then-some */
    CHECK(sol_rope_remove(r, 0, 1000));
    CHECK_EQ_SZ(sol_rope_byte_len(r), 0);

    sol_rope_destroy(r);
}

static void test_line_index(void)
{
    const char *s = "alpha\nbeta\ngamma\ndelta";   /* 3 newlines, 3 lines */
    SolRope *r = sol_rope_from_bytes((const uint8_t *)s, strlen(s));
    CHECK_EQ_SZ(sol_rope_line_count(r), 3);
    CHECK_EQ_SZ(sol_rope_byte_of_line(r, 0), 0);
    CHECK_EQ_SZ(sol_rope_byte_of_line(r, 1), 6);
    CHECK_EQ_SZ(sol_rope_byte_of_line(r, 2), 11);
    CHECK_EQ_SZ(sol_rope_byte_of_line(r, 3), 17);

    CHECK_EQ_SZ(sol_rope_line_of_byte(r, 0), 0);
    CHECK_EQ_SZ(sol_rope_line_of_byte(r, 5), 0);   /* '\n' itself is on line 0 */
    CHECK_EQ_SZ(sol_rope_line_of_byte(r, 6), 1);
    CHECK_EQ_SZ(sol_rope_line_of_byte(r, 17), 3);
    CHECK_EQ_SZ(sol_rope_line_of_byte(r, 999), 3);
    sol_rope_destroy(r);
}

static void test_chunk_iter(void)
{
    /* Force multiple leaves by exceeding LEAF_TARGET (4096). */
    size_t n = 20000;
    uint8_t *src = (uint8_t *)malloc(n);
    for (size_t i = 0; i < n; ++i) src[i] = (uint8_t)('A' + (i % 26));
    SolRope *r = sol_rope_from_bytes(src, n);
    CHECK(r != NULL);

    SolRopeChunkIter it;
    sol_rope_chunk_iter_init(&it, r);
    const uint8_t *p; size_t k, off;
    size_t total = 0;
    size_t prev_off = 0;
    bool first = true;
    while (sol_rope_chunk_iter_next(&it, &p, &k, &off)) {
        CHECK(p != NULL);
        if (!first) CHECK_EQ_SZ(off, prev_off);
        CHECK(memcmp(p, src + off, k) == 0);
        prev_off = off + k;
        total += k;
        first = false;
    }
    CHECK_EQ_SZ(total, n);

    free(src);
    sol_rope_destroy(r);
}

static void test_large_random_edits(void)
{
    /* Mirror rope and an in-memory string through random insert/remove
       operations and assert the byte content matches at every step. */
    size_t cap = 1u << 16;
    char *mirror = (char *)malloc(cap);
    size_t mirror_len = 0;

    SolRope *r = sol_rope_create();
    CHECK(r != NULL);

    srand(424242u);
    for (int op = 0; op < 200; ++op) {
        bool do_insert = (rand() & 1) || mirror_len == 0;
        if (do_insert) {
            size_t pos = mirror_len ? (size_t)rand() % (mirror_len + 1) : 0;
            size_t len = 1 + (size_t)rand() % 64;
            if (mirror_len + len >= cap) {
                cap *= 2;
                mirror = (char *)realloc(mirror, cap);
            }
            uint8_t buf[64];
            for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)('a' + rand() % 26);
            CHECK(sol_rope_insert(r, pos, buf, len));
            memmove(mirror + pos + len, mirror + pos, mirror_len - pos);
            memcpy(mirror + pos, buf, len);
            mirror_len += len;
        } else {
            size_t pos = (size_t)rand() % mirror_len;
            size_t len = 1 + (size_t)rand() % (mirror_len - pos);
            CHECK(sol_rope_remove(r, pos, len));
            memmove(mirror + pos, mirror + pos + len, mirror_len - pos - len);
            mirror_len -= len;
        }
        CHECK_EQ_SZ(sol_rope_byte_len(r), mirror_len);
        size_t got;
        uint8_t *buf = rope_to_buffer(r, &got);
        CHECK_EQ_SZ(got, mirror_len);
        if (got != mirror_len || memcmp(buf, mirror, mirror_len) != 0) {
            fprintf(stderr, "mismatch at op=%d (rope_len=%zu mirror_len=%zu)\n",
                    op, got, mirror_len);
            g_failures++;
            free(buf);
            break;
        }
        free(buf);
    }

    free(mirror);
    sol_rope_destroy(r);
}

static void test_from_file(void)
{
    /* Write a temp file with a known multi-MB payload, mmap it through
       the rope, and verify metrics + reads. */
    char path[] = "/tmp/sol_rope_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;

    size_t n = 1u << 18;   /* 256 KiB */
    char *src = (char *)malloc(n);
    size_t lines = 0;
    for (size_t i = 0; i < n; ++i) {
        if ((i % 80) == 79) { src[i] = '\n'; lines++; }
        else                  src[i] = (char)('a' + (i % 26));
    }
    write(fd, src, n);
    close(fd);

    const char *err = NULL;
    SolRope *r = sol_rope_from_file(path, &err);
    CHECK(r != NULL);
    if (r) {
        CHECK_EQ_SZ(sol_rope_byte_len(r), n);
        CHECK_EQ_SZ(sol_rope_line_count(r), lines);

        /* Sample a few random reads. */
        for (int t = 0; t < 32; ++t) {
            size_t off = (size_t)rand() % n;
            size_t want = 1 + (size_t)rand() % 500;
            if (off + want > n) want = n - off;
            uint8_t buf[512];
            size_t got = sol_rope_read(r, off, buf, want);
            CHECK_EQ_SZ(got, want);
            CHECK(memcmp(buf, src + off, want) == 0);
        }
        sol_rope_destroy(r);
    }
    unlink(path);
    free(src);
}

int main(void)
{
    test_empty();
    test_from_bytes_metrics();
    test_insert();
    test_remove();
    test_line_index();
    test_chunk_iter();
    test_large_random_edits();
    test_from_file();

    if (g_failures > 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "all rope tests passed\n");
    return 0;
}
