// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "test_harness.h"

#include "sol_platform.h"
#include "sol_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define SOL_TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define SOL_TEST_RMDIR rmdir
#endif

static bool write_text_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    const size_t n = strlen(text);
    const bool ok = fwrite(text, 1u, n, file) == n;
    fclose(file);
    return ok;
}

static void test_fuzzy_score(SolTestCtx *T)
{
    const int tight = sol_search_fuzzy_score("src/search_window.c", "swc");
    const int loose = sol_search_fuzzy_score("src/something_with_content.c", "swc");
    SOL_CHECK(T, tight >= 0);
    SOL_CHECK(T, loose >= 0);
    SOL_CHECK(T, tight > loose);
    SOL_CHECK(T, sol_search_fuzzy_score("src/main.c", "zz") < 0);
}

typedef struct SearchProgressCtx {
    size_t calls;
    size_t processed;
    size_t total;
    size_t cancel_after;
    size_t cancel_after_calls;
} SearchProgressCtx;

static bool record_search_progress(size_t processed, size_t total, void *user_data)
{
    SearchProgressCtx *ctx = (SearchProgressCtx *)user_data;
    ctx->calls++;
    ctx->processed = processed;
    ctx->total = total;
    if (ctx->cancel_after_calls > 0u && ctx->calls >= ctx->cancel_after_calls) {
        return false;
    }
    return ctx->cancel_after == 0u || processed < ctx->cancel_after;
}

static void test_workspace_search(SolTestCtx *T)
{
    const char *root = "sol_search_test_workspace";
    const char *src = "sol_search_test_workspace/src";
    const char *build = "sol_search_test_workspace/build";
    const char *hidden = "sol_search_test_workspace/.hidden";
    SOL_CHECK(T, sol_platform_mkdir_p(src));
    SOL_CHECK(T, sol_platform_mkdir_p(build));
    SOL_CHECK(T, sol_platform_mkdir_p(hidden));

    SOL_CHECK(T, write_text_file(
        "sol_search_test_workspace/src/search_window.c",
        "first line\nNeedle appears here\nlast line\n"));
    SOL_CHECK(T, write_text_file(
        "sol_search_test_workspace/src/main.c",
        "needle on line one\n"));
    SOL_CHECK(T, write_text_file(
        "sol_search_test_workspace/build/generated.c",
        "needle should be ignored\n"));
    SOL_CHECK(T, write_text_file(
        "sol_search_test_workspace/.hidden/private.c",
        "needle should be ignored\n"));

    SolSearchIndex *index = sol_search_index_create(root);
    SOL_CHECK_NOT_NULL(T, index);
    if (index) {
        SOL_CHECK_EQ_SZ(T, sol_search_index_file_count(index), 2u);

        SolSearchResult results[8];
        size_t count = sol_search_files(index, "swc", results, 8u);
        SOL_CHECK(T, count >= 1u);
        if (count > 0u) {
            SOL_CHECK_STR(T, results[0].relative_path, "src/search_window.c");
            SOL_CHECK_EQ_SZ(T, results[0].line_number, 0u);
        }

        count = sol_search_contents(index, "NEEDLE", results, 8u);
        SOL_CHECK_EQ_SZ(T, count, 2u);
        bool found_line_two = false;
        for (size_t i = 0u; i < count; ++i) {
            if (strcmp(results[i].relative_path, "src/search_window.c") == 0) {
                found_line_two = results[i].line_number == 2u;
            }
        }
        SOL_CHECK(T, found_line_two);

        SearchProgressCtx progress = {0};
        count = sol_search_contents_progress(
            index, "needle", results, 8u, record_search_progress, &progress);
        SOL_CHECK_EQ_SZ(T, count, 2u);
        SOL_CHECK(T, progress.calls > 0u);
        SOL_CHECK_EQ_SZ(T, progress.processed, 2u);
        SOL_CHECK_EQ_SZ(T, progress.total, 2u);

        SearchProgressCtx cancelled = { .cancel_after = 1u };
        count = sol_search_contents_progress(
            index, "needle", results, 8u, record_search_progress, &cancelled);
        SOL_CHECK(T, count <= 1u);
        SOL_CHECK_EQ_SZ(T, cancelled.processed, 1u);
        SOL_CHECK_EQ_SZ(T, cancelled.total, 2u);
        sol_search_index_destroy(index);
    }

    remove("sol_search_test_workspace/src/search_window.c");
    remove("sol_search_test_workspace/src/main.c");
    remove("sol_search_test_workspace/build/generated.c");
    remove("sol_search_test_workspace/.hidden/private.c");
    SOL_TEST_RMDIR(src);
    SOL_TEST_RMDIR(build);
    SOL_TEST_RMDIR(hidden);
    SOL_TEST_RMDIR(root);
}

static void test_large_line_cancellation(SolTestCtx *T)
{
    const char *root = "sol_search_test_large_line";
    const char *path = "sol_search_test_large_line/large.txt";
    SOL_CHECK(T, sol_platform_mkdir_p(root));

    const size_t size = 256u * 1024u;
    char *text = (char *)malloc(size + 1u);
    SOL_CHECK_NOT_NULL(T, text);
    if (text) {
        memset(text, 'a', size);
        text[size] = '\0';
        SOL_CHECK(T, write_text_file(path, text));
        free(text);
    }

    SolSearchIndex *index = sol_search_index_create(root);
    SOL_CHECK_NOT_NULL(T, index);
    if (index) {
        SolSearchResult results[4];
        SearchProgressCtx cancelled = { .cancel_after_calls = 2u };
        const size_t count = sol_search_contents_progress(
            index, "not-present", results, 4u,
            record_search_progress, &cancelled);
        SOL_CHECK_EQ_SZ(T, count, 0u);
        SOL_CHECK_EQ_SZ(T, cancelled.calls, 2u);
        SOL_CHECK_EQ_SZ(T, cancelled.processed, 0u);
        SOL_CHECK_EQ_SZ(T, cancelled.total, 1u);
        sol_search_index_destroy(index);
    }

    remove(path);
    SOL_TEST_RMDIR(root);
}

int main(void)
{
    SolTestSuite suite;
    sol_suite_init(&suite, "sol_search_tests");
    SOL_RUN(suite, test_fuzzy_score);
    SOL_RUN(suite, test_workspace_search);
    SOL_RUN(suite, test_large_line_cancellation);
    return sol_suite_report(&suite);
}
