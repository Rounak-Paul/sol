#include "git_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++g_failures; \
    } \
} while (0)

/* Verify ordinary, staged, untracked, and branch metadata parsing. */
static void test_status_core_records(void)
{
    static const char status[] =
        "# branch.oid 0123456789abcdef\0"
        "# branch.head main\0"
        "# branch.upstream origin/main\0"
        "# branch.ab +2 -1\0"
        "1 .M N... 100644 100644 100644 abcdef0 abcdef0 src/main.c\0"
        "1 A. N... 000000 100644 100644 0000000 abcdef1 new file.txt\0"
        "? notes/todo.txt\0";
    /* Heap-allocated: GitSnapshot embeds GIT_MAX_FILES (512) entries with two
       GIT_PATH_CAP (4096-byte) paths each, ~4.2 MB total — comfortably inside
       Linux/macOS's 8 MB default thread stack but well past Windows' 1 MB
       default, where a stack-local instance reliably overflows. */
    GitSnapshot *snapshot = (GitSnapshot *)calloc(1u, sizeof(GitSnapshot));
    CHECK(snapshot);
    if (!snapshot) return;
    snprintf(snapshot->root, sizeof(snapshot->root), "/repo");
    char error[GIT_ERROR_CAP] = {0};

    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 snapshot, error, sizeof(error)));
    CHECK(snapshot->repository);
    CHECK(strcmp(snapshot->root, "/repo") == 0);
    CHECK(strcmp(snapshot->branch, "main") == 0);
    CHECK(strcmp(snapshot->upstream, "origin/main") == 0);
    CHECK(snapshot->ahead == 2);
    CHECK(snapshot->behind == 1);
    CHECK(snapshot->file_count == 3u);
    CHECK(snapshot->staged_count == 1u);
    CHECK(snapshot->unstaged_count == 2u);
    CHECK(snapshot->untracked_count == 1u);
    CHECK(strcmp(snapshot->files[0].path, "src/main.c") == 0);
    CHECK(strcmp(snapshot->files[1].path, "new file.txt") == 0);
    free(snapshot);
}

/* Verify rename records consume their second NUL-delimited path. */
static void test_status_rename_record(void)
{
    static const char status[] =
        "# branch.head feature/rename\0"
        "2 R. N... 100644 100644 100644 abcdef0 abcdef1 R100 renamed file.c\0"
        "old file.c\0";
    GitSnapshot *snapshot = (GitSnapshot *)calloc(1u, sizeof(GitSnapshot));
    CHECK(snapshot);
    if (!snapshot) return;
    char error[GIT_ERROR_CAP] = {0};

    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 snapshot, error, sizeof(error)));
    CHECK(snapshot->file_count == 1u);
    CHECK(snapshot->staged_count == 1u);
    CHECK(snapshot->files[0].kind == GIT_FILE_RENAMED);
    CHECK(strcmp(snapshot->files[0].path, "renamed file.c") == 0);
    CHECK(strcmp(snapshot->files[0].original_path, "old file.c") == 0);
    free(snapshot);
}

/* Verify detached and unmerged records are represented safely. */
static void test_status_detached_and_unmerged(void)
{
    static const char status[] =
        "# branch.head (detached)\0"
        "u UU N... 100644 100644 100644 100644 aaaaaaa bbbbbbb ccccccc conflict.txt\0";
    GitSnapshot *snapshot = (GitSnapshot *)calloc(1u, sizeof(GitSnapshot));
    CHECK(snapshot);
    if (!snapshot) return;
    char error[GIT_ERROR_CAP] = {0};

    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 snapshot, error, sizeof(error)));
    CHECK(snapshot->detached);
    CHECK(strcmp(snapshot->branch, "detached") == 0);
    CHECK(snapshot->file_count == 1u);
    CHECK(snapshot->files[0].kind == GIT_FILE_UNMERGED);
    CHECK(snapshot->staged_count == 1u);
    CHECK(snapshot->unstaged_count == 1u);
    free(snapshot);
}

/* Verify malformed records fail without partially trusting the input. */
static void test_status_rejects_malformed_record(void)
{
    static const char status[] = "1 malformed\0";
    GitSnapshot *snapshot = (GitSnapshot *)calloc(1u, sizeof(GitSnapshot));
    CHECK(snapshot);
    if (!snapshot) return;
    char error[GIT_ERROR_CAP] = {0};

    CHECK(!git_model_parse_status(status, sizeof(status) - 1u,
                                  snapshot, error, sizeof(error)));
    CHECK(error[0] != '\0');
    free(snapshot);
}

/* Verify submodule content state is preserved from porcelain-v2 records. */
static void test_status_submodule_record(void)
{
    static const char status[] =
        "1 .M S.MU 160000 160000 160000 abcdef0 abcdef1 vendors/causality\0";
    GitSnapshot *snapshot = (GitSnapshot *)calloc(1u, sizeof(GitSnapshot));
    CHECK(snapshot);
    if (!snapshot) return;
    char error[GIT_ERROR_CAP] = {0};
    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 snapshot, error, sizeof(error)));
    CHECK(snapshot->file_count == 1u);
    CHECK(snapshot->files[0].submodule);
    CHECK(snapshot->files[0].submodule_modified);
    CHECK(snapshot->files[0].submodule_untracked);
    free(snapshot);
}

/* Verify history records remain exact after Git's NUL record terminator. */
static void test_history_nul_records(void)
{
    static const char history_data[] =
        "C2\0C2\0Ada\0" "2026-09-01\0C1\0second\0\0"
        "C1\0C1\0Ada\0" "2026-08-31\0\0first\0\0";
    GitHistory history;
    char error[GIT_ERROR_CAP] = {0};
    CHECK(git_model_parse_history(history_data, sizeof(history_data) - 1u,
                                  &history, error, sizeof(error)));
    CHECK(history.count == 2u);
    CHECK(strcmp(history.commits[0].hash, "C2") == 0);
    CHECK(strcmp(history.commits[1].hash, "C1") == 0);
    CHECK(history.commits[0].lane == 0);
    CHECK(history.commits[1].lane == 0);
}

/* Verify the portable subprocess layer can resolve and capture Git itself. */
static void test_process_git_version(void)
{
    char output[512];
    const char *argv[] = { "git", "--version", NULL };
    GitProcessResult result = git_process_run(".", argv, output,
                                              sizeof(output), 10000u);
    CHECK(result.exit_code == 0);
    CHECK(!result.timed_out);
    CHECK(!result.truncated);
    CHECK(strncmp(output, "git version ", 12u) == 0);
}

/* Verify repository commands use Git's explicit working-directory option. */
static void test_process_git_status(void)
{
    char output[4096];
    const char *argv[] = {
        "git", "status", "--porcelain=v2", "--branch", "-z", NULL,
    };
    GitProcessResult result = git_process_run(".", argv, output,
                                              sizeof(output), 10000u);
    CHECK(result.exit_code == 0);
    CHECK(result.exit_code != 126);
    CHECK(!result.timed_out);
}

/* Verify refresh discovers the workspace's registered recursive submodules. */
static void test_refresh_submodules(void)
{
    char root[GIT_PATH_CAP] = {0};
    char error[GIT_ERROR_CAP] = {0};
    GitSnapshot *snapshot = (GitSnapshot *)calloc(1u, sizeof(*snapshot));
    CHECK(snapshot);
    if (!snapshot) return;
    CHECK(git_model_discover(".", root, sizeof(root), error, sizeof(error)));
    if (root[0]) {
        CHECK(git_model_refresh(root, snapshot, error, sizeof(error)));
        CHECK(snapshot->submodule_count > 0u);
        bool found_causality = false;
        for (size_t i = 0u; i < snapshot->submodule_count; ++i) {
            if (strcmp(snapshot->submodules[i].path, "vendors/causality") == 0) {
                found_causality = true;
                break;
            }
        }
        CHECK(found_causality);
    }
    free(snapshot);
}

/* Verify an invalid repository path is reported by Git, not as launch code 126. */
static void test_process_invalid_directory(void)
{
    char output[1024];
    const char *argv[] = { "git", "status", "--porcelain=v2", NULL };
    GitProcessResult result = git_process_run(
        "__sol_git_plugin_directory_that_does_not_exist__", argv, output,
        sizeof(output), 10000u);
    CHECK(result.exit_code != 0);
    CHECK(result.exit_code != 126);
    CHECK(!result.timed_out);
    CHECK(output[0] != '\0');
}

/* Verify git diff works on a tracked modified file using an absolute cwd. */
static void test_process_tracked_diff(void)
{
    char repo_root[4096];
    const char *root_argv[] = { "git", "rev-parse", "--show-toplevel", NULL };
    GitProcessResult root_result = git_process_run(".", root_argv,
                                                   repo_root, sizeof(repo_root),
                                                   10000u);
    CHECK(root_result.exit_code == 0);
    if (root_result.exit_code != 0) return;
    repo_root[strcspn(repo_root, "\r\n")] = '\0';

    /* Heap-allocated: 64 KB + 1 MB of stack buffers in this one frame would
       overflow Windows' 1 MB default thread stack (fine under Linux/macOS's
       8 MB default — see the GitSnapshot comment in test_status_core_records
       for the same pattern). */
    char *status_output = (char *)malloc(65536u);
    CHECK(status_output);
    if (!status_output) return;
    const char *status_argv[] = {
        "git", "status", "--porcelain=v2", "-z", "--untracked-files=all", NULL
    };
    GitProcessResult status_result = git_process_run(repo_root, status_argv,
                                                     status_output,
                                                     65536u,
                                                     10000u);
    CHECK(status_result.exit_code == 0);
    if (status_result.exit_code != 0) { free(status_output); return; }

    const char *modified_path = NULL;
    const char *cursor = status_output;
    const char *end = status_output + status_result.output_len;
    while (cursor < end) {
        size_t record_len = strlen(cursor);
        if (record_len >= 2u && cursor[0] == '1' && cursor[1] == ' ') {
            const char *p = cursor;
            int field = 0;
            while (p < cursor + record_len && field < 8) {
                p = strchr(p, ' ');
                if (!p) break;
                ++p;
                ++field;
            }
            if (field == 8 && p && *p) {
                modified_path = p;
                break;
            }
        }
        cursor += record_len + 1u;
    }

    if (!modified_path) {
        printf("test_process_tracked_diff: no modified files, skipping\n");
        free(status_output);
        return;
    }

    char *diff_output = (char *)malloc(1048576u);
    CHECK(diff_output);
    if (!diff_output) { free(status_output); return; }
    const char *diff_argv[] = {
        "git", "diff", "--no-ext-diff", "--", modified_path, NULL
    };
    GitProcessResult diff_result = git_process_run(repo_root, diff_argv,
                                                   diff_output,
                                                   1048576u,
                                                   30000u);
    printf("test_process_tracked_diff: repo_root='%s' path='%s' exit=%d len=%zu\n",
           repo_root, modified_path, diff_result.exit_code,
           diff_result.output_len);
    CHECK(diff_result.exit_code == 0);
    CHECK(diff_result.output_len > 0u);
    free(diff_output);
    free(status_output);
}

/* Verify Git can render a complete patch for a file absent from the index. */
static void test_process_untracked_diff(void)
{
    char repo_root[4096];
    const char *root_argv[] = { "git", "rev-parse", "--show-toplevel", NULL };
    GitProcessResult root_result = git_process_run(".", root_argv,
                                                   repo_root, sizeof(repo_root),
                                                   10000u);
    CHECK(root_result.exit_code == 0);
    if (root_result.exit_code != 0) return;
    repo_root[strcspn(repo_root, "\r\n")] = '\0';

    char target_path[8192];
    int target_written = snprintf(target_path, sizeof(target_path),
                                  "%s/%s", repo_root,
                                  "plugins/sol-plugin-git/tests/test_git_model.c");
    CHECK(target_written > 0);
    CHECK((size_t)target_written < sizeof(target_path));
    if (target_written <= 0 || (size_t)target_written >= sizeof(target_path)) return;

    char output[65536];
#if defined(_WIN32)
    const char *null_path = "NUL";
#else
    const char *null_path = "/dev/null";
#endif
    const char *argv[] = {
        "git", "diff", "--no-index", "--no-ext-diff", "--",
        null_path, target_path, NULL,
    };
    GitProcessResult result = git_process_run(".", argv, output,
                                              sizeof(output), 10000u);
    CHECK(result.exit_code == 1);
    CHECK(!result.timed_out);
    CHECK(!result.truncated);
    CHECK(strncmp(output, "diff --git ", 11u) == 0);
    CHECK(strstr(output, "--- /dev/null") != NULL ||
          strstr(output, "--- a/NUL") != NULL);
}

/* Fill one commit slot with a hash and up to two parent hashes. */
static void set_commit(GitCommitEntry *entry, const char *hash,
                       const char *parent0, const char *parent1)
{
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->hash, sizeof(entry->hash), "%s", hash);
    if (parent0) {
        snprintf(entry->parents[0], sizeof(entry->parents[0]), "%s", parent0);
        entry->parent_count = 1u;
    }
    if (parent1) {
        snprintf(entry->parents[1], sizeof(entry->parents[1]), "%s", parent1);
        entry->parent_count = 2u;
    }
}

/* Verify a merge commit and its feature branch get distinct lanes that
 * converge back to the mainline lane once the branch's base is reached. */
static void test_graph_layout_merge(void)
{
    GitHistory history;
    memset(&history, 0, sizeof(history));
    /* Newest-first, matching git log order:
     *   C5 = merge(C4, C3b), C4 = feature-of(C2)->main, C3b = feature tip,
     *   C2 = shared base, C1 = root. */
    set_commit(&history.commits[0], "C5", "C4", "C3b");
    set_commit(&history.commits[1], "C4", "C2", NULL);
    set_commit(&history.commits[2], "C3b", "C2", NULL);
    set_commit(&history.commits[3], "C2", "C1", NULL);
    set_commit(&history.commits[4], "C1", NULL, NULL);
    history.count = 5u;

    git_model_layout_graph(&history);

    CHECK(history.commits[0].lane == 0); /* C5: merge commit stays on mainline */
    CHECK(history.commits[1].lane == 0); /* C4: mainline continues */
    CHECK(history.commits[2].lane == 1); /* C3b: feature branch gets its own lane */
    CHECK(history.commits[3].lane == 0); /* C2: both lanes converge back */
    CHECK(history.commits[4].lane == 0); /* C1: root, still mainline */
    /* C4's row is where the feature lane is visibly open alongside main. */
    CHECK((history.commits[1].through_lanes & 0x3u) == 0x3u);
    CHECK(history.commits[0].parent_lanes[0] == 0);
    CHECK(history.commits[0].parent_lanes[1] == 1);
}

/* Verify two unrelated branch tips (never merged, as happens when history
 * is truncated) don't hold a lane open past where each one closes — the
 * second branch should reuse the first's freed lane, not open a new one. */
static void test_graph_layout_unrelated_branches(void)
{
    GitHistory history;
    memset(&history, 0, sizeof(history));
    set_commit(&history.commits[0], "B2", "B1", NULL);
    set_commit(&history.commits[1], "B1", NULL, NULL);
    set_commit(&history.commits[2], "A2", "A1", NULL);
    set_commit(&history.commits[3], "A1", NULL, NULL);
    history.count = 4u;

    git_model_layout_graph(&history);

    CHECK(history.commits[0].lane == 0);
    CHECK(history.commits[1].lane == 0);
    CHECK(history.commits[2].lane == 0); /* reuses lane 0, freed after B1 */
    CHECK(history.commits[3].lane == 0);
}

int main(void)
{
    test_status_core_records();
    test_status_rename_record();
    test_status_detached_and_unmerged();
    test_status_rejects_malformed_record();
    test_status_submodule_record();
    test_history_nul_records();
    test_process_git_version();
    test_process_git_status();
    test_refresh_submodules();
    test_process_invalid_directory();
    test_process_tracked_diff();
    test_process_untracked_diff();
    test_graph_layout_merge();
    test_graph_layout_unrelated_branches();
    if (g_failures == 0) {
        printf("sol_git_plugin_tests: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "sol_git_plugin_tests: %d failure(s)\n", g_failures);
    return 1;
}
