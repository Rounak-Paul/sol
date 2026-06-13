#include "git_plugin.h"

#include <stdio.h>
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
    GitSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snprintf(snapshot.root, sizeof(snapshot.root), "/repo");
    char error[GIT_ERROR_CAP] = {0};

    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 &snapshot, error, sizeof(error)));
    CHECK(snapshot.repository);
    CHECK(strcmp(snapshot.root, "/repo") == 0);
    CHECK(strcmp(snapshot.branch, "main") == 0);
    CHECK(strcmp(snapshot.upstream, "origin/main") == 0);
    CHECK(snapshot.ahead == 2);
    CHECK(snapshot.behind == 1);
    CHECK(snapshot.file_count == 3u);
    CHECK(snapshot.staged_count == 1u);
    CHECK(snapshot.unstaged_count == 2u);
    CHECK(snapshot.untracked_count == 1u);
    CHECK(strcmp(snapshot.files[0].path, "src/main.c") == 0);
    CHECK(strcmp(snapshot.files[1].path, "new file.txt") == 0);
}

/* Verify rename records consume their second NUL-delimited path. */
static void test_status_rename_record(void)
{
    static const char status[] =
        "# branch.head feature/rename\0"
        "2 R. N... 100644 100644 100644 abcdef0 abcdef1 R100 renamed file.c\0"
        "old file.c\0";
    GitSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    char error[GIT_ERROR_CAP] = {0};

    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 &snapshot, error, sizeof(error)));
    CHECK(snapshot.file_count == 1u);
    CHECK(snapshot.staged_count == 1u);
    CHECK(snapshot.files[0].kind == GIT_FILE_RENAMED);
    CHECK(strcmp(snapshot.files[0].path, "renamed file.c") == 0);
    CHECK(strcmp(snapshot.files[0].original_path, "old file.c") == 0);
}

/* Verify detached and unmerged records are represented safely. */
static void test_status_detached_and_unmerged(void)
{
    static const char status[] =
        "# branch.head (detached)\0"
        "u UU N... 100644 100644 100644 100644 aaaaaaa bbbbbbb ccccccc conflict.txt\0";
    GitSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    char error[GIT_ERROR_CAP] = {0};

    CHECK(git_model_parse_status(status, sizeof(status) - 1u,
                                 &snapshot, error, sizeof(error)));
    CHECK(snapshot.detached);
    CHECK(strcmp(snapshot.branch, "detached") == 0);
    CHECK(snapshot.file_count == 1u);
    CHECK(snapshot.files[0].kind == GIT_FILE_UNMERGED);
    CHECK(snapshot.staged_count == 1u);
    CHECK(snapshot.unstaged_count == 1u);
}

/* Verify malformed records fail without partially trusting the input. */
static void test_status_rejects_malformed_record(void)
{
    static const char status[] = "1 malformed\0";
    GitSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    char error[GIT_ERROR_CAP] = {0};

    CHECK(!git_model_parse_status(status, sizeof(status) - 1u,
                                  &snapshot, error, sizeof(error)));
    CHECK(error[0] != '\0');
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

int main(void)
{
    test_status_core_records();
    test_status_rename_record();
    test_status_detached_and_unmerged();
    test_status_rejects_malformed_record();
    test_process_git_version();
    test_process_git_status();
    test_process_invalid_directory();
    if (g_failures == 0) {
        printf("sol_git_plugin_tests: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "sol_git_plugin_tests: %d failure(s)\n", g_failures);
    return 1;
}
