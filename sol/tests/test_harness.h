// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* test_harness.h — Lightweight single-header test framework for Sol.
 *
 * Design goals:
 *   - Zero external dependencies (libc only).
 *   - Performance measurement via monotonic clock.
 *   - Structured output: per-suite summaries + overall report.
 *   - Automatically excluded from Release builds via SOL_TESTS_ENABLED.
 *
 * Usage:
 *   #include "test_harness.h"
 *
 *   static void test_foo(SolTestCtx *T) {
 *       SOL_CHECK(T, 1 + 1 == 2);
 *       SOL_CHECK_EQ_SZ(T, strlen("hi"), 2);
 *       SOL_CHECK_STR(T, "hello", "hello");
 *   }
 *
 *   int main(void) {
 *       SolTestSuite s;
 *       sol_suite_init(&s, "MySuite");
 *       SOL_RUN(s, test_foo);
 *       return sol_suite_report(&s);
 *   }
 */

#ifndef SOL_TEST_HARNESS_H
#define SOL_TEST_HARNESS_H

#ifndef SOL_TESTS_ENABLED
/* Excluded in Release mode; define SOL_TESTS_ENABLED=1 in Debug/Test CMake. */
#  ifdef NDEBUG
#    error "Sol tests should not be compiled in Release mode. Define SOL_TESTS_ENABLED only in Debug/Test."
#  endif
#  define SOL_TESTS_ENABLED 1
#endif

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Platform time                                                       */
/* ------------------------------------------------------------------ */

static inline uint64_t sol_test_now_ns(void)
{
#if defined(__APPLE__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#elif defined(_WIN32)
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)(cnt.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ */
/* Context + suite types                                               */
/* ------------------------------------------------------------------ */

#define SOL_TEST_MAX_NAME     128
#define SOL_TEST_MAX_FAILURES 64

typedef struct SolTestFailure {
    char file[SOL_TEST_MAX_NAME];
    int  line;
    char message[256];
} SolTestFailure;

typedef struct SolTestCtx {
    const char      *name;
    int              failures;
    int              checks;
    SolTestFailure   failure_list[SOL_TEST_MAX_FAILURES];
    uint64_t         start_ns;
} SolTestCtx;

typedef struct SolTestResult {
    char     name[SOL_TEST_MAX_NAME];
    bool     passed;
    int      checks;
    int      failures;
    uint64_t elapsed_ns;
} SolTestResult;

#define SOL_SUITE_MAX_TESTS 256

typedef struct SolTestSuite {
    char          name[SOL_TEST_MAX_NAME];
    SolTestResult results[SOL_SUITE_MAX_TESTS];
    int           count;
    int           passed;
    int           failed;
    uint64_t      total_ns;
} SolTestSuite;

/* ------------------------------------------------------------------ */
/* Macros                                                              */
/* ------------------------------------------------------------------ */

#define SOL_CHECK(T, cond)                                                \
    do {                                                                   \
        (T)->checks++;                                                     \
        if (!(cond)) {                                                     \
            sol_test_fail(T, __FILE__, __LINE__,                           \
                          "CHECK failed: " #cond);                        \
        }                                                                  \
    } while (0)

#define SOL_CHECK_MSG(T, cond, fmt, ...)                                  \
    do {                                                                   \
        (T)->checks++;                                                     \
        if (!(cond)) {                                                     \
            sol_test_fail_fmt(T, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
        }                                                                  \
    } while (0)

#define SOL_CHECK_EQ_SZ(T, a, b)                                          \
    do {                                                                   \
        (T)->checks++;                                                     \
        size_t _a = (size_t)(a), _b = (size_t)(b);                        \
        if (_a != _b) {                                                    \
            sol_test_fail_fmt(T, __FILE__, __LINE__,                       \
                              "%s == %s  expected %zu, got %zu",           \
                              #a, #b, _b, _a);                            \
        }                                                                  \
    } while (0)

#define SOL_CHECK_EQ_INT(T, a, b)                                         \
    do {                                                                   \
        (T)->checks++;                                                     \
        long long _a = (long long)(a), _b = (long long)(b);               \
        if (_a != _b) {                                                    \
            sol_test_fail_fmt(T, __FILE__, __LINE__,                       \
                              "%s == %s  expected %lld, got %lld",         \
                              #a, #b, _b, _a);                            \
        }                                                                  \
    } while (0)

#define SOL_CHECK_EQ_FLOAT(T, a, b, eps)                                  \
    do {                                                                   \
        (T)->checks++;                                                     \
        double _a = (double)(a), _b = (double)(b), _e = (double)(eps);    \
        if (fabs(_a - _b) > _e) {                                         \
            sol_test_fail_fmt(T, __FILE__, __LINE__,                       \
                              "%s ~= %s  expected %.6f, got %.6f (eps=%.6f)", \
                              #a, #b, _b, _a, _e);                        \
        }                                                                  \
    } while (0)

#define SOL_CHECK_STR(T, a, b)                                            \
    do {                                                                   \
        (T)->checks++;                                                     \
        const char *_a = (const char *)(a);                               \
        const char *_b = (const char *)(b);                               \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                          \
            sol_test_fail_fmt(T, __FILE__, __LINE__,                       \
                              "%s == %s  expected \"%s\", got \"%s\"",    \
                              #a, #b,                                      \
                              _b ? _b : "<null>",                          \
                              _a ? _a : "<null>");                         \
        }                                                                  \
    } while (0)

#define SOL_CHECK_NULL(T, ptr)                                            \
    do {                                                                   \
        (T)->checks++;                                                     \
        if ((ptr) != NULL) {                                               \
            sol_test_fail_fmt(T, __FILE__, __LINE__,                       \
                              "Expected NULL: " #ptr);                    \
        }                                                                  \
    } while (0)

#define SOL_CHECK_NOT_NULL(T, ptr)                                        \
    do {                                                                   \
        (T)->checks++;                                                     \
        if ((ptr) == NULL) {                                               \
            sol_test_fail_fmt(T, __FILE__, __LINE__,                       \
                              "Expected non-NULL: " #ptr);                \
        }                                                                  \
    } while (0)

/* Register and run a test function. */
#define SOL_RUN(suite, fn)                                                \
    do {                                                                   \
        SolTestCtx _ctx;                                                   \
        _ctx.name     = #fn;                                               \
        _ctx.failures = 0;                                                 \
        _ctx.checks   = 0;                                                 \
        _ctx.start_ns = sol_test_now_ns();                                 \
        fn(&_ctx);                                                         \
        sol_suite_record(&(suite), &_ctx);                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* Implementation                                                      */
/* ------------------------------------------------------------------ */

static inline void sol_test_fail(SolTestCtx *T, const char *file, int line,
                                  const char *msg)
{
    if (T->failures < SOL_TEST_MAX_FAILURES) {
        SolTestFailure *f = &T->failure_list[T->failures];
        snprintf(f->file, sizeof(f->file), "%s", file);
        f->line = line;
        snprintf(f->message, sizeof(f->message), "%s", msg);
    }
    T->failures++;
}

static inline void sol_test_fail_fmt(SolTestCtx *T, const char *file, int line,
                                      const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sol_test_fail(T, file, line, buf);
}

static inline void sol_suite_init(SolTestSuite *s, const char *name)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
}

static inline void sol_suite_record(SolTestSuite *s, const SolTestCtx *ctx)
{
    if (s->count >= SOL_SUITE_MAX_TESTS) return;
    const uint64_t elapsed = sol_test_now_ns() - ctx->start_ns;
    SolTestResult *r = &s->results[s->count++];
    snprintf(r->name, sizeof(r->name), "%s", ctx->name);
    r->passed    = (ctx->failures == 0);
    r->checks    = ctx->checks;
    r->failures  = ctx->failures;
    r->elapsed_ns = elapsed;
    s->total_ns  += elapsed;
    if (r->passed) {
        s->passed++;
    } else {
        s->failed++;
        /* Print failures immediately so they are visible in CI logs. */
        fprintf(stderr, "  [FAIL] %s\n", ctx->name);
        for (int i = 0; i < ctx->failures && i < SOL_TEST_MAX_FAILURES; ++i) {
            const SolTestFailure *f = &ctx->failure_list[i];
            fprintf(stderr, "    %s:%d  %s\n", f->file, f->line, f->message);
        }
    }
}

static inline int sol_suite_report(const SolTestSuite *s)
{
    const double ms = (double)s->total_ns / 1e6;
    fprintf(stderr, "\n=== %s  %d/%d passed  %.2f ms ===\n",
            s->name, s->passed, s->count, ms);
    for (int i = 0; i < s->count; ++i) {
        const SolTestResult *r = &s->results[i];
        fprintf(stderr, "  %s %-52s %5d checks  %6.2f ms\n",
                r->passed ? "[PASS]" : "[FAIL]",
                r->name,
                r->checks,
                (double)r->elapsed_ns / 1e6);
    }
    if (s->failed > 0) {
        fprintf(stderr, "  %d FAILURE(S)\n", s->failed);
    }
    return s->failed > 0 ? 1 : 0;
}

/* Run multiple suites and print an aggregate report. Returns nonzero
   on any failure. */
static inline int sol_report_all(const SolTestSuite *suites, int count)
{
    int total_pass = 0, total_fail = 0, total_checks = 0;
    uint64_t total_ns = 0;
    for (int i = 0; i < count; ++i) {
        total_pass   += suites[i].passed;
        total_fail   += suites[i].failed;
        total_ns     += suites[i].total_ns;
        for (int j = 0; j < suites[i].count; ++j)
            total_checks += suites[i].results[j].checks;
    }
    const double ms = (double)total_ns / 1e6;
    fprintf(stderr,
            "\n╔══════════════════════════════════════════════════════════╗\n");
    fprintf(stderr,
            "║  SOL TEST REPORT  %3d passed  %3d failed  %5d checks   ║\n",
            total_pass, total_fail, total_checks);
    fprintf(stderr,
            "║  Total time: %.2f ms                                     ║\n",
            ms);
    fprintf(stderr,
            "╚══════════════════════════════════════════════════════════╝\n");
    return total_fail > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Performance benchmark helper                                        */
/* ------------------------------------------------------------------ */

/* Run `fn(user_data)` `iters` times and print min/avg/max ns per iter. */
static inline void sol_bench(const char *name, int iters,
                              void (*fn)(void *), void *user_data)
{
    uint64_t min_ns  = UINT64_MAX;
    uint64_t max_ns  = 0;
    uint64_t total   = 0;

    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = sol_test_now_ns();
        fn(user_data);
        uint64_t t1 = sol_test_now_ns();
        uint64_t dt = t1 - t0;
        if (dt < min_ns) min_ns = dt;
        if (dt > max_ns) max_ns = dt;
        total += dt;
    }

    const double avg_ns = (double)total / (double)iters;
    fprintf(stderr,
            "  [BENCH] %-40s  iters=%d  min=%.0fns  avg=%.0fns  max=%.0fns\n",
            name, iters, (double)min_ns, avg_ns, (double)max_ns);
}

#endif /* SOL_TEST_HARNESS_H */
