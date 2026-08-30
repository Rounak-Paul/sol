#ifndef SOL_GIT_PLUGIN_H
#define SOL_GIT_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sol_buffer.h"
#include "sol_event.h"
#include "sol_plugin_ctx.h"

#define GIT_PATH_CAP 4096u
#define GIT_MESSAGE_CAP 1024u
#define GIT_ERROR_CAP 1024u
#define GIT_PROCESS_OUTPUT_CAP (4u * 1024u * 1024u)
#define GIT_MAX_FILES 512u
#define GIT_MAX_COMMITS 128u
#define GIT_MAX_BRANCHES 256u
#define GIT_TASK_TIMEOUT_MS 120000u

typedef enum GitFileKind {
    GIT_FILE_ORDINARY = 0,
    GIT_FILE_RENAMED,
    GIT_FILE_UNMERGED,
    GIT_FILE_UNTRACKED,
} GitFileKind;

typedef struct GitFileStatus {
    char path[GIT_PATH_CAP];
    char original_path[GIT_PATH_CAP];
    char index_status;
    char worktree_status;
    GitFileKind kind;
} GitFileStatus;

#define GIT_MAX_PARENTS 2u
#define GIT_MAX_GRAPH_LANES 32u

typedef struct GitCommitEntry {
    char hash[41];
    char short_hash[17];
    char author[128];
    char date[32];
    char subject[512];
    /* Parent commit hashes (first GIT_MAX_PARENTS of them; octopus merges
     * beyond that are rare and the graph only needs enough parents to draw
     * merge/branch lines, not full ancestry). parent_count may be 0 (root
     * commit). */
    char parents[GIT_MAX_PARENTS][41];
    size_t parent_count;
    /* Graph layout, computed by git_model_layout_graph() after history is
     * loaded: which vertical lane this commit's dot sits in, and which
     * lanes have a line passing through this row (bit i set = lane i has
     * a continuing line segment spanning this row, used to draw the
     * vertical connectors between a parent lane and where it resumes). */
    int lane;
    uint32_t through_lanes;
} GitCommitEntry;

typedef struct GitBranchEntry {
    char name[256];
    char upstream[256];
    bool current;
} GitBranchEntry;

typedef struct GitSnapshot {
    bool repository;
    bool detached;
    char root[GIT_PATH_CAP];
    char branch[256];
    char upstream[256];
    int ahead;
    int behind;
    GitFileStatus files[GIT_MAX_FILES];
    size_t file_count;
    size_t staged_count;
    size_t unstaged_count;
    size_t untracked_count;
    size_t omitted_count;
} GitSnapshot;

typedef struct GitHistory {
    GitCommitEntry commits[GIT_MAX_COMMITS];
    size_t count;
} GitHistory;

typedef struct GitBranches {
    GitBranchEntry branches[GIT_MAX_BRANCHES];
    size_t count;
} GitBranches;

typedef struct GitProcessResult {
    int exit_code;
    bool timed_out;
    bool truncated;
    size_t output_len;
} GitProcessResult;

typedef enum GitTaskKind {
    GIT_TASK_DISCOVER = 0,
    GIT_TASK_REFRESH,
    GIT_TASK_HISTORY,
    GIT_TASK_BRANCHES,
    GIT_TASK_STAGE,
    GIT_TASK_UNSTAGE,
    GIT_TASK_DISCARD,
    GIT_TASK_STAGE_ALL,
    GIT_TASK_UNSTAGE_ALL,
    GIT_TASK_COMMIT,
    GIT_TASK_FETCH,
    GIT_TASK_PULL,
    GIT_TASK_PUSH,
    GIT_TASK_CHECKOUT,
    GIT_TASK_CREATE_BRANCH,
    GIT_TASK_INIT,
    GIT_TASK_DIFF,
    GIT_TASK_SHOW_COMMIT,
    GIT_TASK_BLAME,
} GitTaskKind;

typedef enum GitDiffMode {
    GIT_DIFF_UNSTAGED = 0,
    GIT_DIFF_STAGED,
    GIT_DIFF_UNTRACKED,
} GitDiffMode;

typedef struct GitTask {
    GitTaskKind kind;
    SolPluginCtx *ctx;
    void *owner;
    char workspace_root[GIT_PATH_CAP];
    char repo_root[GIT_PATH_CAP];
    char argument[GIT_PATH_CAP];
    char secondary[GIT_PATH_CAP];
    bool flag;
    GitDiffMode diff_mode;
    GitSnapshot snapshot;
    GitHistory history;
    GitBranches branches;
    char *output;
    size_t output_len;
    int exit_code;
    bool timed_out;
    bool truncated;
    char error[GIT_ERROR_CAP];
} GitTask;

/* Run Git with an argv array in cwd and capture merged stdout/stderr. */
GitProcessResult git_process_run(const char *cwd,
                                 const char *const argv[],
                                 char *output,
                                 size_t output_capacity,
                                 uint32_t timeout_ms);

/* Discover the containing repository for path. */
bool git_model_discover(const char *path,
                        char *root,
                        size_t root_capacity,
                        char *error,
                        size_t error_capacity);

/* Refresh branch and working-tree status from porcelain-v2 output. */
bool git_model_refresh(const char *root,
                       GitSnapshot *snapshot,
                       char *error,
                       size_t error_capacity);

/* Load recent commit history, including parent hashes for graph layout. */
bool git_model_history(const char *root,
                       GitHistory *history,
                       char *error,
                       size_t error_capacity);

/* Assign a vertical lane to every commit in history (already in reverse-
 * chronological order, i.e. newest first, matching `git log`'s default
 * order) and record which lanes have a continuing line through each row,
 * so the history view can render a compact branch/merge graph. Commits
 * are assumed already populated with parent hashes. Exposed for tests. */
void git_model_layout_graph(GitHistory *history);

/* Load local branches and upstream relationships. */
bool git_model_branches(const char *root,
                        GitBranches *branches,
                        char *error,
                        size_t error_capacity);

/* Parse porcelain-v2 status bytes into a snapshot. Exposed for tests. */
bool git_model_parse_status(const char *data,
                            size_t length,
                            GitSnapshot *snapshot,
                            char *error,
                            size_t error_capacity);

/* Open captured Git output in a colored, read-only custom buffer. */
SolBufferId git_view_open(SolPluginCtx *ctx,
                          const char *title,
                          const char *text,
                          size_t text_length);

#endif
