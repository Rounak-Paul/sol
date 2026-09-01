#include "git_plugin.h"

#include <causality.h>

#include "sol_plugin.h"
#include "sol_text_buffer.h"
#include "sol_ui_system.h"

#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIT_ACTION_CONTEXT_CAP 4096u

typedef enum GitPanelTab {
    GIT_PANEL_CHANGES = 0,
    GIT_PANEL_HISTORY,
    GIT_PANEL_BRANCHES,
} GitPanelTab;

typedef enum GitUiAction {
    GIT_UI_CLOSE = 0,
    GIT_UI_REFRESH,
    GIT_UI_FETCH,
    GIT_UI_PULL,
    GIT_UI_PUSH,
    GIT_UI_CHANGES,
    GIT_UI_HISTORY,
    GIT_UI_BRANCHES,
    GIT_UI_STAGE,
    GIT_UI_UNSTAGE,
    GIT_UI_DISCARD,
    GIT_UI_CONFIRM_DISCARD,
    GIT_UI_CANCEL_DISCARD,
    GIT_UI_OPEN,
    GIT_UI_DIFF,
    GIT_UI_STAGE_ALL,
    GIT_UI_UNSTAGE_ALL,
    GIT_UI_COMMIT,
    GIT_UI_SHOW_COMMIT,
    GIT_UI_CHECKOUT,
    GIT_UI_CREATE_BRANCH,
    GIT_UI_INIT,
    GIT_UI_SELECT_REPOSITORY,
    GIT_UI_SELECT_WORKSPACE_REPOSITORY,
} GitUiAction;

typedef struct GitPlugin GitPlugin;

typedef struct GitActionContext {
    GitPlugin *plugin;
    GitUiAction action;
    bool flag;
    const char *value;
} GitActionContext;

struct GitPlugin {
    SolPluginCtx *ctx;
    SolPluginSidePanelToken panel_token;
    SolPluginStatusToken status_token;
    SolJobFence *task_fence;
    GitTask *task;
    _Atomic bool task_done;
    bool task_running;
    bool shutting_down;

    char workspace_root[GIT_PATH_CAP];
    GitSnapshot snapshot;
    GitHistory history;
    GitBranches branches;
    GitPanelTab tab;

    char commit_message[GIT_MESSAGE_CAP];
    char new_branch[256];
    char active_file[GIT_PATH_CAP];
    char error[GIT_ERROR_CAP];
    char activity[128];
    char pending_discard[GIT_PATH_CAP];
    bool pending_discard_untracked;

    Ca_TextInput *commit_input;
    Ca_TextInput *branch_input;
    bool needs_commit_focus;
    bool rediscover_pending;
    GitActionContext actions[GIT_ACTION_CONTEXT_CAP];
    size_t action_count;

    SolSubscriptionToken root_subscription;
    SolSubscriptionToken focus_subscription;
};

/* Copy a string into a fixed destination. */
static void git_copy_string(char *destination,
                            size_t capacity,
                            const char *source)
{
    if (!destination || capacity == 0u) return;
    snprintf(destination, capacity, "%s", source ? source : "");
}

/* Return true when a status has an index-side change. */
static bool git_file_is_staged(const GitFileStatus *file)
{
    return file && file->kind != GIT_FILE_UNTRACKED &&
           file->index_status != '.' && file->index_status != ' ';
}

/* Return true when a status has a worktree-side change. */
static bool git_file_is_unstaged(const GitFileStatus *file)
{
    return file && (file->kind == GIT_FILE_UNTRACKED ||
                    file->kind == GIT_FILE_UNMERGED ||
                    (file->worktree_status != '.' && file->worktree_status != ' '));
}

/* Select the patch source represented by one source-control row. */
static GitDiffMode git_diff_mode(const GitPlugin *plugin,
                                 const char *path,
                                 bool staged)
{
    if (!plugin || !path) return staged ? GIT_DIFF_STAGED : GIT_DIFF_UNSTAGED;
    for (size_t i = 0u; i < plugin->snapshot.file_count; ++i) {
        const GitFileStatus *file = &plugin->snapshot.files[i];
        if (strcmp(file->path, path) != 0) continue;
        if (file->kind == GIT_FILE_UNTRACKED) return GIT_DIFF_UNTRACKED;
        if (staged || !git_file_is_unstaged(file)) return GIT_DIFF_STAGED;
        return GIT_DIFF_UNSTAGED;
    }
    return staged ? GIT_DIFF_STAGED : GIT_DIFF_UNSTAGED;
}

/* Return a compact human-readable status label. */
static const char *git_file_status_label(const GitFileStatus *file,
                                         bool staged)
{
    if (!file) return "?";
    if (file->kind == GIT_FILE_UNTRACKED) return "U";
    if (file->kind == GIT_FILE_UNMERGED) return "!";
    char code = staged ? file->index_status : file->worktree_status;
    switch (code) {
        case 'A': return "A";
        case 'D': return "D";
        case 'M': return "M";
        case 'R': return "R";
        case 'C': return "C";
        case 'T': return "T";
        default: return "?";
    }
}

/* Return a style class for a file status. */
static const char *git_file_status_style(const GitFileStatus *file,
                                         bool staged)
{
    const char *label = git_file_status_label(file, staged);
    if (label[0] == 'A' || label[0] == 'U') return "scm-status-added";
    if (label[0] == 'D') return "scm-status-deleted";
    if (label[0] == '!') return "scm-status-conflict";
    if (label[0] == 'R' || label[0] == 'C') return "scm-status-renamed";
    return "scm-status-modified";
}

/* Return the final path component (file name) of a repository-relative path. */
static const char *git_basename(const char *path)
{
    if (!path) return "";
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    return slash ? slash + 1 : path;
}

/* Return true when `suffix` (some tail segment of `path`, always ending at
 * its basename) also matches the same tail of `other` at a clean path-
 * segment boundary. Used to test whether a shortened display name would
 * still be ambiguous against another visible file. */
static bool git_path_suffix_collides(const char *suffix, const char *other)
{
    const size_t suffix_len = strlen(suffix);
    const size_t other_len = strlen(other);
    if (other_len < suffix_len) return false;
    const char *other_suffix = other + (other_len - suffix_len);
    if (strcmp(suffix, other_suffix) != 0) return false;
    if (other_len == suffix_len) return true;
    const char sep = other[other_len - suffix_len - 1u];
    return sep == '/' || sep == '\\';
}

/* Return true when some other file in the snapshot shares the tail `suffix`
 * of `path`. */
static bool git_suffix_ambiguous(const GitSnapshot *snapshot,
                                 const char *path,
                                 const char *suffix)
{
    for (size_t i = 0u; i < snapshot->file_count; ++i) {
        const char *other = snapshot->files[i].path;
        if (strcmp(other, path) == 0) continue;
        if (git_path_suffix_collides(suffix, other)) return true;
    }
    return false;
}

/* Build the display name for one file row into `out`: the basename alone,
 * or however many trailing path segments are needed to distinguish it from
 * every other visible file sharing that basename (e.g. two plugins each
 * with their own src/plugin.c both need their plugin directory name, not
 * just "src", to stay distinguishable). Falls back to the full path if
 * every ancestor segment is exhausted without becoming unique. */
static void git_display_name(const GitSnapshot *snapshot,
                             const char *path,
                             char *out,
                             size_t out_capacity)
{
    const char *seg_start = git_basename(path);
    while (git_suffix_ambiguous(snapshot, path, seg_start) && seg_start > path) {
        const char *sep = seg_start - 1;
        const char *seg_begin = sep;
        while (seg_begin > path && seg_begin[-1] != '/' && seg_begin[-1] != '\\') {
            --seg_begin;
        }
        seg_start = seg_begin;
    }
    git_copy_string(out, out_capacity, seg_start);
}

/* Allocate a stable click context for the current panel rebuild. */
static GitActionContext *git_action_context(GitPlugin *plugin,
                                            GitUiAction action,
                                            const char *value,
                                            bool flag)
{
    if (!plugin || plugin->action_count >= GIT_ACTION_CONTEXT_CAP) return NULL;
    GitActionContext *context = &plugin->actions[plugin->action_count++];
    memset(context, 0, sizeof(*context));
    context->plugin = plugin;
    context->action = action;
    context->flag = flag;
    context->value = value;
    return context;
}

/* Append bytes to a task-owned output buffer. */
static void git_task_append(GitTask *task, const char *text, size_t length)
{
    if (!task || !task->output || !text || length == 0u) return;
    const size_t capacity = GIT_PROCESS_OUTPUT_CAP;
    const size_t available = task->output_len < capacity - 1u
        ? capacity - 1u - task->output_len : 0u;
    const size_t copy = length < available ? length : available;
    if (copy > 0u) memcpy(task->output + task->output_len, text, copy);
    task->output_len += copy;
    task->output[task->output_len] = '\0';
    if (copy < length) task->truncated = true;
}

/* Run one command and retain a concise error on failure. */
static bool git_task_command(GitTask *task,
                             const char *cwd,
                             const char *const argv[],
                             uint32_t timeout_ms)
{
    char *output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!output) {
        snprintf(task->error, sizeof(task->error), "Out of memory starting Git");
        task->exit_code = -1;
        return false;
    }
    GitProcessResult result = git_process_run(cwd, argv, output,
                                              GIT_PROCESS_OUTPUT_CAP,
                                              timeout_ms);
    task->exit_code = result.exit_code;
    task->timed_out = result.timed_out;
    task->truncated = result.truncated;
    if (result.exit_code != 0 || result.truncated) {
        if (result.timed_out) {
            snprintf(task->error, sizeof(task->error), "Git operation timed out");
        } else if (result.truncated) {
            snprintf(task->error, sizeof(task->error), "Git output exceeded %u bytes",
                     GIT_PROCESS_OUTPUT_CAP - 1u);
        } else if (output[0]) {
            size_t line = strcspn(output, "\r\n");
            snprintf(task->error, sizeof(task->error), "%.*s", (int)line, output);
        } else {
            snprintf(task->error, sizeof(task->error),
                     "Git failed with exit code %d", result.exit_code);
        }
        free(output);
        return false;
    }
    free(output);
    return true;
}

/* Capture one command's full output for a read-only view. */
static bool git_task_capture(GitTask *task,
                             const char *cwd,
                             const char *const argv[],
                             uint32_t timeout_ms)
{
    task->output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!task->output) {
        snprintf(task->error, sizeof(task->error), "Out of memory capturing Git output");
        return false;
    }
    GitProcessResult result = git_process_run(cwd, argv, task->output,
                                              GIT_PROCESS_OUTPUT_CAP,
                                              timeout_ms);
    task->output_len = result.output_len;
    task->exit_code = result.exit_code;
    task->timed_out = result.timed_out;
    task->truncated = result.truncated;
    if (result.exit_code == 0 && !result.truncated) return true;
    if (result.timed_out) {
        snprintf(task->error, sizeof(task->error), "Git operation timed out");
    } else if (result.truncated) {
        snprintf(task->error, sizeof(task->error), "Git output exceeded %u bytes",
                 GIT_PROCESS_OUTPUT_CAP - 1u);
    } else if (task->output[0]) {
        size_t line = strcspn(task->output, "\r\n");
        snprintf(task->error, sizeof(task->error), "%.*s", (int)line, task->output);
    } else {
        snprintf(task->error, sizeof(task->error),
                 "Git failed with exit code %d", result.exit_code);
    }
    return false;
}

/* Refresh task snapshot after a successful repository mutation. */
static bool git_task_refresh_snapshot(GitTask *task)
{
    return git_model_refresh(task->repo_root, &task->snapshot,
                             task->error, sizeof(task->error));
}

/* Execute a staged-file reset with an unborn-branch fallback. */
static bool git_task_unstage(GitTask *task, const char *path)
{
    const char *restore_all[] = { "git", "restore", "--staged", "--", ".", NULL };
    const char *restore_one[] = { "git", "restore", "--staged", "--", path, NULL };
    char head_output[256];
    const char *verify_head[] = { "git", "rev-parse", "--verify", "HEAD", NULL };
    GitProcessResult head = git_process_run(task->repo_root, verify_head,
                                            head_output, sizeof(head_output), 10000u);
    if (head.exit_code == 0) {
        return git_task_command(task, task->repo_root,
                                path ? restore_one : restore_all, 30000u);
    }
    if (head.exit_code == 127 || head.timed_out) {
        snprintf(task->error, sizeof(task->error),
                 head.timed_out ? "Git HEAD check timed out" : "Git executable was not found");
        task->exit_code = head.exit_code;
        return false;
    }

    const char *remove_all[] = {
        "git", "rm", "-r", "--cached", "--ignore-unmatch", "--", ".", NULL
    };
    const char *remove_one[] = {
        "git", "rm", "--cached", "--ignore-unmatch", "--", path, NULL
    };
    return git_task_command(task, task->repo_root,
                            path ? remove_one : remove_all, 30000u);
}

/* Execute the patch represented by one source-control row. */
static bool git_task_diff(GitTask *task)
{
    task->output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    char *part = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!task->output || !part) {
        free(part);
        snprintf(task->error, sizeof(task->error), "Out of memory capturing diff");
        return false;
    }

    const char *const unstaged[] = {
        "git", "diff", "--no-ext-diff", "--", task->argument, NULL
    };
    const char *const staged[] = {
        "git", "diff", "--cached", "--no-ext-diff", "--",
        task->argument, NULL
    };
#if defined(_WIN32)
    const char *null_path = "NUL";
#else
    const char *null_path = "/dev/null";
#endif
    const char *untracked[] = {
        "git", "diff", "--no-index", "--no-ext-diff", "--",
        null_path, task->argument, NULL
    };
    const char *const *argv = unstaged;
    const char *heading = "UNSTAGED CHANGES\n\n";
    bool accepts_difference_exit = false;
    if (task->diff_mode == GIT_DIFF_STAGED) {
        argv = staged;
        heading = "STAGED CHANGES\n\n";
    } else if (task->diff_mode == GIT_DIFF_UNTRACKED) {
        argv = untracked;
        heading = "UNTRACKED FILE\n\n";
        accepts_difference_exit = true;
    }

    GitProcessResult result = git_process_run(task->repo_root, argv, part,
                                              GIT_PROCESS_OUTPUT_CAP, 30000u);
    const bool command_ok = result.exit_code == 0 ||
        (accepts_difference_exit && result.exit_code == 1);
    if (!command_ok || result.truncated) {
        if (result.timed_out) {
            snprintf(task->error, sizeof(task->error), "Git diff timed out");
        } else if (result.truncated) {
            snprintf(task->error, sizeof(task->error), "Git diff output is too large");
        } else if (part[0]) {
            size_t line = strcspn(part, "\r\n");
            snprintf(task->error, sizeof(task->error), "%.*s", (int)line, part);
        } else {
            snprintf(task->error, sizeof(task->error),
                     "Git diff failed with exit code %d", result.exit_code);
        }
        task->exit_code = result.exit_code;
        task->timed_out = result.timed_out;
        free(part);
        return false;
    }
    if (result.output_len > 0u) {
        git_task_append(task, heading, strlen(heading));
        git_task_append(task, part, result.output_len);
    }
    if (task->output_len == 0u) {
        static const char empty[] = "No diff is available for this path.\n";
        git_task_append(task, empty, sizeof(empty) - 1u);
    }
    free(part);
    return !task->truncated;
}

/* Execute one serialized Git task on a worker thread. */
static void git_task_worker(void *user_data)
{
    GitTask *task = (GitTask *)user_data;
    GitPlugin *plugin = task ? (GitPlugin *)task->owner : NULL;
    bool success = false;
    if (!task || !plugin) return;

    switch (task->kind) {
        case GIT_TASK_DISCOVER:
            success = git_model_discover(task->argument[0] ? task->argument
                                                           : task->workspace_root,
                                         task->snapshot.root,
                                         sizeof(task->snapshot.root),
                                         task->error, sizeof(task->error));
            if (success) {
                success = git_model_refresh(task->snapshot.root, &task->snapshot,
                                            task->error, sizeof(task->error));
            }
            break;
        case GIT_TASK_REFRESH:
            success = git_model_refresh(task->repo_root, &task->snapshot,
                                        task->error, sizeof(task->error));
            break;
        case GIT_TASK_HISTORY:
            success = git_model_history(task->repo_root, &task->history,
                                        task->error, sizeof(task->error));
            break;
        case GIT_TASK_BRANCHES:
            success = git_model_branches(task->repo_root, &task->branches,
                                         task->error, sizeof(task->error));
            break;
        case GIT_TASK_STAGE: {
            const char *argv[] = { "git", "add", "--", task->argument, NULL };
            success = git_task_command(task, task->repo_root, argv, 30000u) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_UNSTAGE:
            success = git_task_unstage(task, task->argument) &&
                      git_task_refresh_snapshot(task);
            break;
        case GIT_TASK_DISCARD: {
            const char *clean[] = { "git", "clean", "-f", "--", task->argument, NULL };
            const char *restore[] = {
                "git", "restore", "--worktree", "--", task->argument, NULL
            };
            success = git_task_command(task, task->repo_root,
                                       task->flag ? clean : restore, 30000u) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_STAGE_ALL: {
            const char *argv[] = { "git", "add", "-A", NULL };
            success = git_task_command(task, task->repo_root, argv, 30000u) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_UNSTAGE_ALL:
            success = git_task_unstage(task, NULL) && git_task_refresh_snapshot(task);
            break;
        case GIT_TASK_COMMIT: {
            const char *argv[] = { "git", "commit", "-m", task->argument, NULL };
            success = git_task_command(task, task->repo_root, argv, 60000u) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_FETCH: {
            const char *argv[] = { "git", "fetch", "--prune", NULL };
            success = git_task_command(task, task->repo_root, argv,
                                       GIT_TASK_TIMEOUT_MS) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_PULL: {
            const char *argv[] = { "git", "pull", "--ff-only", NULL };
            success = git_task_command(task, task->repo_root, argv,
                                       GIT_TASK_TIMEOUT_MS) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_PUSH: {
            const char *push[] = { "git", "push", NULL };
            const char *push_upstream[] = {
                "git", "push", "--set-upstream", "origin", task->secondary, NULL
            };
            success = git_task_command(task, task->repo_root,
                                       task->flag ? push_upstream : push,
                                       GIT_TASK_TIMEOUT_MS) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_CHECKOUT: {
            const char *argv[] = { "git", "switch", task->argument, NULL };
            success = git_task_command(task, task->repo_root, argv, 30000u) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_CREATE_BRANCH: {
            const char *argv[] = { "git", "switch", "-c", task->argument, NULL };
            success = git_task_command(task, task->repo_root, argv, 30000u) &&
                      git_task_refresh_snapshot(task);
            break;
        }
        case GIT_TASK_INIT: {
            const char *argv[] = { "git", "init", NULL };
            success = git_task_command(task, task->workspace_root, argv, 30000u) &&
                      git_model_discover(task->workspace_root, task->repo_root,
                                         sizeof(task->repo_root), task->error,
                                         sizeof(task->error)) &&
                      git_model_refresh(task->repo_root, &task->snapshot,
                                        task->error, sizeof(task->error));
            break;
        }
        case GIT_TASK_DIFF:
            success = git_task_diff(task);
            break;
        case GIT_TASK_SHOW_COMMIT: {
            const char *argv[] = {
                "git", "show", "--decorate", "--stat", "--patch",
                "--format=fuller", task->argument, NULL
            };
            success = git_task_capture(task, task->repo_root, argv, 30000u);
            break;
        }
        case GIT_TASK_BLAME: {
            const char *argv[] = {
                "git", "blame", "--date=short", "--", task->argument, NULL
            };
            success = git_task_capture(task, task->repo_root, argv, 30000u);
            break;
        }
    }

    if (success) task->exit_code = 0;
    else if (task->exit_code == 0) task->exit_code = 1;
    atomic_store_explicit(&plugin->task_done, true, memory_order_release);
    sol_plugin_wake_ui(task->ctx);
}

/* Return a short activity label for a task kind. */
static const char *git_task_activity(GitTaskKind kind)
{
    switch (kind) {
        case GIT_TASK_DISCOVER: return "Finding repository...";
        case GIT_TASK_REFRESH: return "Refreshing changes...";
        case GIT_TASK_HISTORY: return "Loading history...";
        case GIT_TASK_BRANCHES: return "Loading branches...";
        case GIT_TASK_STAGE: return "Staging file...";
        case GIT_TASK_UNSTAGE: return "Unstaging file...";
        case GIT_TASK_DISCARD: return "Discarding changes...";
        case GIT_TASK_STAGE_ALL: return "Staging all changes...";
        case GIT_TASK_UNSTAGE_ALL: return "Unstaging all changes...";
        case GIT_TASK_COMMIT: return "Creating commit...";
        case GIT_TASK_FETCH: return "Fetching remotes...";
        case GIT_TASK_PULL: return "Pulling changes...";
        case GIT_TASK_PUSH: return "Pushing changes...";
        case GIT_TASK_CHECKOUT: return "Switching branch...";
        case GIT_TASK_CREATE_BRANCH: return "Creating branch...";
        case GIT_TASK_INIT: return "Initializing repository...";
        case GIT_TASK_DIFF: return "Loading diff...";
        case GIT_TASK_SHOW_COMMIT: return "Loading commit...";
        case GIT_TASK_BLAME: return "Loading blame...";
    }
    return "Running Git...";
}

/* Submit one task if no other Git operation is in flight. */
static bool git_start_task(GitPlugin *plugin,
                           GitTaskKind kind,
                           const char *argument,
                           bool flag)
{
    if (!plugin || plugin->shutting_down || plugin->task_running) return false;
    if (kind != GIT_TASK_DISCOVER && kind != GIT_TASK_INIT &&
        !plugin->snapshot.repository) return false;

    GitTask *task = (GitTask *)calloc(1u, sizeof(*task));
    if (!task) {
        snprintf(plugin->error, sizeof(plugin->error), "Out of memory starting Git task");
        return false;
    }
    task->kind = kind;
    task->ctx = plugin->ctx;
    task->owner = plugin;
    task->flag = flag;
    if (kind == GIT_TASK_DIFF) {
        task->diff_mode = git_diff_mode(plugin, argument, flag);
    }
    git_copy_string(task->workspace_root, sizeof(task->workspace_root),
                    plugin->workspace_root);
    git_copy_string(task->repo_root, sizeof(task->repo_root),
                    plugin->snapshot.root);
    git_copy_string(task->argument, sizeof(task->argument), argument);
    git_copy_string(task->secondary, sizeof(task->secondary),
                    plugin->snapshot.branch);
    if (kind == GIT_TASK_PUSH) task->flag = !plugin->snapshot.upstream[0];

    atomic_store_explicit(&plugin->task_done, false, memory_order_relaxed);
    plugin->task = task;
    plugin->task_running = true;
    plugin->error[0] = '\0';
    git_copy_string(plugin->activity, sizeof(plugin->activity),
                    git_task_activity(kind));
    if (!sol_plugin_submit_job(plugin->ctx, git_task_worker, task,
                               plugin->task_fence)) {
        plugin->task = NULL;
        plugin->task_running = false;
        free(task);
        snprintf(plugin->error, sizeof(plugin->error), "Git job queue is full");
        return false;
    }
    sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
    return true;
}

/* Update the status-bar branch summary from the current snapshot. */
static void git_update_status_segment(GitPlugin *plugin)
{
    if (!plugin || plugin->status_token == SOL_PLUGIN_STATUS_TOKEN_INVALID) return;
    char status[256];
    if (!plugin->snapshot.repository) {
        snprintf(status, sizeof(status), "git: no repository");
    } else if (plugin->snapshot.ahead > 0 || plugin->snapshot.behind > 0) {
        snprintf(status, sizeof(status), "%s  +%zu ~%zu  up %d down %d",
                 plugin->snapshot.detached ? "detached" : plugin->snapshot.branch,
                 plugin->snapshot.staged_count,
                 plugin->snapshot.unstaged_count,
                 plugin->snapshot.ahead,
                 plugin->snapshot.behind);
    } else {
        snprintf(status, sizeof(status), "%s  +%zu ~%zu",
                 plugin->snapshot.detached ? "detached" : plugin->snapshot.branch,
                 plugin->snapshot.staged_count,
                 plugin->snapshot.unstaged_count);
    }
    sol_plugin_update_status_segment(plugin->ctx, plugin->status_token, status);
}

/* Build an absolute repository path from a relative status path. */
static bool git_absolute_path(const GitPlugin *plugin,
                              const char *relative,
                              char *output,
                              size_t output_capacity)
{
    if (!plugin || !relative || !output || output_capacity == 0u ||
        !plugin->snapshot.root[0]) return false;
    int written = snprintf(output, output_capacity, "%s/%s",
                           plugin->snapshot.root, relative);
    return written > 0 && (size_t)written < output_capacity;
}

/* Adopt a completed task and publish its result to reactive UI state. */
static void git_consume_task(GitPlugin *plugin)
{
    if (!plugin || !plugin->task_running ||
        !atomic_load_explicit(&plugin->task_done, memory_order_acquire)) return;

    sol_job_fence_wait(plugin->task_fence);
    GitTask *task = plugin->task;
    plugin->task = NULL;
    plugin->task_running = false;
    plugin->activity[0] = '\0';

    if (!task) return;
    const bool stale_repository = plugin->rediscover_pending ||
        (task->kind == GIT_TASK_DISCOVER &&
         strcmp(task->workspace_root, plugin->workspace_root) != 0);
    if (!stale_repository && task->exit_code == 0) {
        plugin->error[0] = '\0';
        switch (task->kind) {
            case GIT_TASK_DISCOVER: {
                const bool repository_changed =
                    strcmp(plugin->snapshot.root, task->snapshot.root) != 0;
                plugin->snapshot = task->snapshot;
                if (repository_changed) {
                    memset(&plugin->history, 0, sizeof(plugin->history));
                    memset(&plugin->branches, 0, sizeof(plugin->branches));
                    plugin->commit_message[0] = '\0';
                    plugin->new_branch[0] = '\0';
                    plugin->pending_discard[0] = '\0';
                }
                break;
            }
            case GIT_TASK_REFRESH:
            case GIT_TASK_STAGE:
            case GIT_TASK_UNSTAGE:
            case GIT_TASK_DISCARD:
            case GIT_TASK_STAGE_ALL:
            case GIT_TASK_UNSTAGE_ALL:
            case GIT_TASK_COMMIT:
            case GIT_TASK_FETCH:
            case GIT_TASK_PULL:
            case GIT_TASK_PUSH:
            case GIT_TASK_CHECKOUT:
            case GIT_TASK_CREATE_BRANCH:
            case GIT_TASK_INIT:
                plugin->snapshot = task->snapshot;
                if (task->kind != GIT_TASK_REFRESH &&
                    task->kind != GIT_TASK_DISCOVER) {
                    memset(&plugin->history, 0, sizeof(plugin->history));
                }
                if (task->kind == GIT_TASK_CHECKOUT ||
                    task->kind == GIT_TASK_CREATE_BRANCH) {
                    memset(&plugin->branches, 0, sizeof(plugin->branches));
                }
                if (task->kind == GIT_TASK_COMMIT) plugin->commit_message[0] = '\0';
                if (task->kind == GIT_TASK_CREATE_BRANCH) plugin->new_branch[0] = '\0';
                break;
            case GIT_TASK_HISTORY:
                plugin->history = task->history;
                break;
            case GIT_TASK_BRANCHES:
                plugin->branches = task->branches;
                break;
            case GIT_TASK_DIFF: {
                char title[320];
                snprintf(title, sizeof(title), "Diff: %s", task->argument);
                (void)git_view_open(plugin->ctx, title, task->output,
                                    task->output_len);
                break;
            }
            case GIT_TASK_SHOW_COMMIT: {
                char title[96];
                snprintf(title, sizeof(title), "Commit %.12s", task->argument);
                (void)git_view_open(plugin->ctx, title, task->output,
                                    task->output_len);
                break;
            }
            case GIT_TASK_BLAME: {
                const char *name = strrchr(task->argument, '/');
                char title[320];
                snprintf(title, sizeof(title), "Blame: %s",
                         name ? name + 1 : task->argument);
                (void)git_view_open(plugin->ctx, title, task->output,
                                    task->output_len);
                break;
            }
        }
    } else if (!stale_repository && task->kind == GIT_TASK_DISCOVER) {
        memset(&plugin->snapshot, 0, sizeof(plugin->snapshot));
        if (task->exit_code == 127) {
            git_copy_string(plugin->error, sizeof(plugin->error),
                            "Git executable was not found");
        } else {
            plugin->error[0] = '\0';
        }
    } else if (!stale_repository) {
        git_copy_string(plugin->error, sizeof(plugin->error), task->error);
    }

    free(task->output);
    free(task);
    if (plugin->rediscover_pending) {
        plugin->rediscover_pending = false;
        if (plugin->workspace_root[0]) {
            (void)git_start_task(plugin, GIT_TASK_DISCOVER, NULL, false);
        }
    }
    git_update_status_segment(plugin);
    sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
}

/* Return true when text contains at least one non-whitespace byte. */
static bool git_has_content(const char *text)
{
    if (!text) return false;
    while (*text) {
        if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n') {
            return true;
        }
        ++text;
    }
    return false;
}

static void git_on_action(Ca_Button *button, void *user_data);

/* Render an action button wired to the common dispatcher. */
static void git_render_button(GitPlugin *plugin,
                              const char *label,
                              GitUiAction action,
                              const char *value,
                              bool flag,
                              bool disabled,
                              const char *style)
{
    GitActionContext *context = git_action_context(plugin, action, value, flag);
    ca_btn_begin(&(Ca_BtnDesc){
        .text = label,
        .on_click = context ? git_on_action : NULL,
        .click_data = context,
        .style = style ? style : "scm-action",
        .disabled = disabled || !context,
    });
    ca_btn_end();
}

/* Render a square icon-only button with a hover tooltip label. Used for
 * compact, densely-aligned row/header actions where a text label would
 * break horizontal rhythm across rows. Every icon button in this panel
 * uses the same 16px box / 11px glyph geometry as .buffer-tab-close —
 * the one icon-button size in this app with a proven non-clipping
 * track record — rather than a size invented for this panel. */
static void git_render_icon_button(GitPlugin *plugin,
                                   const char *icon,
                                   const char *tooltip,
                                   GitUiAction action,
                                   const char *value,
                                   bool flag,
                                   bool disabled,
                                   const char *style)
{
    GitActionContext *context = git_action_context(plugin, action, value, flag);
    ca_btn_begin(&(Ca_BtnDesc){
        .on_click = context ? git_on_action : NULL,
        .click_data = context,
        .style = style ? style : "scm-icon-action",
        .disabled = disabled || !context,
        .direction = CA_HORIZONTAL,
    });
    ca_text(&(Ca_TextDesc){ .text = icon, .style = "scm-icon-glyph" });
    ca_btn_end();
    if (tooltip) ca_tooltip(&(Ca_TooltipDesc){ .text = tooltip });
}

/* Dispatch all source-control panel click actions. */
static void git_on_action(Ca_Button *button, void *user_data)
{
    (void)button;
    GitActionContext *context = (GitActionContext *)user_data;
    GitPlugin *plugin = context ? context->plugin : NULL;
    if (!plugin) return;

    switch (context->action) {
        case GIT_UI_CLOSE:
            sol_plugin_hide_side_panel(plugin->ctx, plugin->panel_token);
            break;
        case GIT_UI_REFRESH:
            (void)git_start_task(plugin, GIT_TASK_REFRESH, NULL, false);
            break;
        case GIT_UI_FETCH:
            (void)git_start_task(plugin, GIT_TASK_FETCH, NULL, false);
            break;
        case GIT_UI_PULL:
            (void)git_start_task(plugin, GIT_TASK_PULL, NULL, false);
            break;
        case GIT_UI_PUSH:
            (void)git_start_task(plugin, GIT_TASK_PUSH, NULL, false);
            break;
        case GIT_UI_CHANGES:
            plugin->tab = GIT_PANEL_CHANGES;
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            break;
        case GIT_UI_HISTORY:
            plugin->tab = GIT_PANEL_HISTORY;
            if (plugin->history.count == 0u) {
                (void)git_start_task(plugin, GIT_TASK_HISTORY, NULL, false);
            }
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            break;
        case GIT_UI_BRANCHES:
            plugin->tab = GIT_PANEL_BRANCHES;
            (void)git_start_task(plugin, GIT_TASK_BRANCHES, NULL, false);
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            break;
        case GIT_UI_STAGE:
            (void)git_start_task(plugin, GIT_TASK_STAGE, context->value, false);
            break;
        case GIT_UI_UNSTAGE:
            (void)git_start_task(plugin, GIT_TASK_UNSTAGE, context->value, false);
            break;
        case GIT_UI_DISCARD:
            git_copy_string(plugin->pending_discard,
                            sizeof(plugin->pending_discard), context->value);
            plugin->pending_discard_untracked = context->flag;
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            break;
        case GIT_UI_CONFIRM_DISCARD:
            if (plugin->pending_discard[0]) {
                (void)git_start_task(plugin, GIT_TASK_DISCARD,
                                     plugin->pending_discard,
                                     plugin->pending_discard_untracked);
            }
            plugin->pending_discard[0] = '\0';
            break;
        case GIT_UI_CANCEL_DISCARD:
            plugin->pending_discard[0] = '\0';
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            break;
        case GIT_UI_OPEN: {
            char path[GIT_PATH_CAP];
            if (git_absolute_path(plugin, context->value, path, sizeof(path))) {
                SolBufferId id = sol_plugin_open_file(plugin->ctx, path);
                if (id != 0u) (void)sol_plugin_focus_buffer(plugin->ctx, id);
            }
            break;
        }
        case GIT_UI_DIFF:
            (void)git_start_task(plugin, GIT_TASK_DIFF,
                                 context->value, context->flag);
            break;
        case GIT_UI_STAGE_ALL:
            (void)git_start_task(plugin, GIT_TASK_STAGE_ALL, NULL, false);
            break;
        case GIT_UI_UNSTAGE_ALL:
            (void)git_start_task(plugin, GIT_TASK_UNSTAGE_ALL, NULL, false);
            break;
        case GIT_UI_COMMIT:
            if (git_has_content(plugin->commit_message)) {
                (void)git_start_task(plugin, GIT_TASK_COMMIT,
                                     plugin->commit_message, false);
            }
            break;
        case GIT_UI_SHOW_COMMIT:
            (void)git_start_task(plugin, GIT_TASK_SHOW_COMMIT,
                                 context->value, false);
            break;
        case GIT_UI_CHECKOUT:
            (void)git_start_task(plugin, GIT_TASK_CHECKOUT,
                                 context->value, false);
            break;
        case GIT_UI_CREATE_BRANCH:
            if (git_has_content(plugin->new_branch)) {
                (void)git_start_task(plugin, GIT_TASK_CREATE_BRANCH,
                                     plugin->new_branch, false);
            }
            break;
        case GIT_UI_INIT:
            (void)git_start_task(plugin, GIT_TASK_INIT, NULL, false);
            break;
        case GIT_UI_SELECT_REPOSITORY: {
            char path[GIT_PATH_CAP];
            if (git_absolute_path(plugin, context->value, path, sizeof(path))) {
                (void)git_start_task(plugin, GIT_TASK_DISCOVER, path, false);
            }
            break;
        }
        case GIT_UI_SELECT_WORKSPACE_REPOSITORY:
            (void)git_start_task(plugin, GIT_TASK_DISCOVER, NULL, false);
            break;
    }
}

/* Mirror commit input edits into plugin-owned state. */
static void git_on_commit_change(Ca_TextInput *input, void *user_data)
{
    GitPlugin *plugin = (GitPlugin *)user_data;
    if (plugin) git_copy_string(plugin->commit_message,
                                sizeof(plugin->commit_message),
                                ca_get_text(input));
}

/* Mirror branch input edits into plugin-owned state. */
static void git_on_branch_change(Ca_TextInput *input, void *user_data)
{
    GitPlugin *plugin = (GitPlugin *)user_data;
    if (plugin) git_copy_string(plugin->new_branch,
                                sizeof(plugin->new_branch),
                                ca_get_text(input));
}

/* Render one changed-file row. The row itself opens the diff (the most
 * common action); a dedicated icon button opens the file for editing, and
 * stage/unstage/discard get their own icon buttons so no action is hidden
 * behind ambiguous click targets. Only the file name is shown inline —
 * the full repository-relative path (and rename source, if any) surfaces
 * as a hover tooltip so long paths never crowd the fixed-width action rail. */
static void git_render_file_row(GitPlugin *plugin,
                                const GitFileStatus *file,
                                bool staged)
{
    const bool row_disabled = plugin->task_running;
    char name[128];
    git_display_name(&plugin->snapshot, file->path, name, sizeof(name));
    const bool renamed = file->kind == GIT_FILE_RENAMED && file->original_path[0];

    GitActionContext *row_ctx = git_action_context(
        plugin, GIT_UI_DIFF, file->path, staged);
    ca_btn_begin(&(Ca_BtnDesc){
        .direction = CA_HORIZONTAL,
        .on_click = row_ctx ? git_on_action : NULL,
        .click_data = row_ctx,
        .style = "scm-file-row",
        .disabled = row_disabled || !row_ctx,
    });

    ca_text(&(Ca_TextDesc){
        .text = git_file_status_label(file, staged),
        .style = git_file_status_style(file, staged),
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-file-name-col",
    });
    if (renamed) {
        ca_text(&(Ca_TextDesc){
            .text = git_basename(file->original_path),
            .style = "scm-file-name-old",
        });
        ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_ARROW_UP, .style = "scm-file-rename-arrow" });
    }
    ca_text(&(Ca_TextDesc){ .text = name, .style = "scm-file-name" });
    ca_div_end();
    {
        char full_path[GIT_PATH_CAP + 64u];
        if (renamed) {
            snprintf(full_path, sizeof(full_path), "%s -> %s",
                     file->original_path, file->path);
        } else {
            git_copy_string(full_path, sizeof(full_path), file->path);
        }
        ca_tooltip(&(Ca_TooltipDesc){ .text = full_path });
    }

    git_render_icon_button(plugin, CA_ICON_NF_COD_GO_TO_FILE, "Open File",
                           GIT_UI_OPEN, file->path, false,
                           false, "scm-icon-action");
    if (staged) {
        git_render_icon_button(plugin, CA_ICON_NF_COD_DASH, "Unstage",
                               GIT_UI_UNSTAGE, file->path, false,
                               row_disabled, "scm-icon-action");
    } else {
        git_render_icon_button(plugin, CA_ICON_NF_COD_ADD, "Stage",
                               GIT_UI_STAGE, file->path, false,
                               row_disabled, "scm-icon-action");
        git_render_icon_button(plugin, CA_ICON_NF_COD_DISCARD,
                               file->kind == GIT_FILE_UNTRACKED
                                   ? "Delete Untracked File" : "Discard Changes",
                               GIT_UI_DISCARD, file->path,
                               file->kind == GIT_FILE_UNTRACKED,
                               row_disabled, "scm-icon-action scm-icon-danger");
    }

    ca_btn_end();   /* scm-file-row */
}

/* Render a staged or unstaged file group. Omitted entirely when empty so a
 * partially-clean tree (e.g. everything staged) doesn't show a dangling
 * zero-count section with nothing underneath it. */
static void git_render_file_group(GitPlugin *plugin,
                                  const char *title,
                                  bool staged)
{
    const size_t count = staged ? plugin->snapshot.staged_count
                                : plugin->snapshot.unstaged_count;
    if (count == 0u) return;

    char heading[128];
    snprintf(heading, sizeof(heading), "%s (%zu)", title, count);
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-section-header",
    });
    ca_text(&(Ca_TextDesc){ .text = heading, .style = "scm-section-title" });
    git_render_button(plugin, staged ? "Unstage All" : "Stage All",
                      staged ? GIT_UI_UNSTAGE_ALL : GIT_UI_STAGE_ALL,
                      NULL, false, plugin->task_running, "scm-section-action");
    ca_div_end();

    for (size_t i = 0u; i < plugin->snapshot.file_count; ++i) {
        const GitFileStatus *file = &plugin->snapshot.files[i];
        if ((staged && git_file_is_staged(file)) ||
            (!staged && git_file_is_unstaged(file))) {
            git_render_file_row(plugin, file, staged);
        }
    }
}

/* Render the changes tab, including commit input and file groups. */
static void git_render_changes(GitPlugin *plugin)
{
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "scm-commit-box",
    });
    plugin->commit_input = ca_input(&(Ca_InputDesc){
        .text = plugin->commit_message,
        .placeholder = plugin->snapshot.staged_count > 0u
            ? "Commit message"
            : "Stage changes to commit",
        .on_change = git_on_commit_change,
        .change_data = plugin,
        .style = "scm-commit-input",
        .disabled = plugin->task_running,
    });
    {
        char commit_label[32];
        if (plugin->snapshot.staged_count > 0u) {
            snprintf(commit_label, sizeof(commit_label), "Commit (%zu)",
                     plugin->snapshot.staged_count);
        } else {
            git_copy_string(commit_label, sizeof(commit_label), "Commit");
        }
        git_render_button(plugin, commit_label, GIT_UI_COMMIT, NULL, false,
                          plugin->task_running || plugin->snapshot.staged_count == 0u ||
                          !git_has_content(plugin->commit_message),
                          "scm-primary-action");
    }
    ca_div_end();

    if (plugin->pending_discard[0]) {
        char message[GIT_PATH_CAP + 96u];
        snprintf(message, sizeof(message), "Discard changes to \"%s\"?%s",
                 git_basename(plugin->pending_discard),
                 plugin->pending_discard_untracked
                     ? " This will permanently delete the untracked file."
                     : " This cannot be undone.");
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "scm-confirm",
        });
        ca_text(&(Ca_TextDesc){ .text = message, .style = "scm-confirm-text" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "scm-confirm-actions" });
        git_render_button(plugin, "Cancel", GIT_UI_CANCEL_DISCARD, NULL, false,
                          false, "scm-action");
        git_render_button(plugin, "Discard", GIT_UI_CONFIRM_DISCARD, NULL, false,
                          false, "scm-danger-action");
        ca_div_end();
        ca_div_end();
    }

    if (plugin->snapshot.file_count == 0u &&
        plugin->snapshot.submodule_count == 0u) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "scm-clean-state" });
        ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_CHECK, .style = "scm-clean-icon" });
        ca_text(&(Ca_TextDesc){ .text = "No changes", .style = "scm-empty" });
        ca_div_end();
        return;
    }
    git_render_file_group(plugin, "Staged Changes", true);
    git_render_file_group(plugin, "Changes", false);
    if (plugin->snapshot.submodule_count > 0u) {
        char heading[128];
        snprintf(heading, sizeof(heading), "Submodules (%zu)",
                 plugin->snapshot.submodule_count);
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style = "scm-section-header",
        });
        ca_text(&(Ca_TextDesc){ .text = heading, .style = "scm-section-title" });
        ca_div_end();
        for (size_t i = 0u; i < plugin->snapshot.submodule_count; ++i) {
            const GitSubmodule *submodule = &plugin->snapshot.submodules[i];
            const char *state = "Clean";
            const char *style = "scm-submodule-clean";
            if (submodule->state == GIT_SUBMODULE_UNINITIALIZED) {
                state = "Uninitialized";
                style = "scm-submodule-warning";
            } else if (submodule->state == GIT_SUBMODULE_REVISION_CHANGED) {
                state = "Revision changed";
                style = "scm-submodule-modified";
            } else if (submodule->state == GIT_SUBMODULE_CONFLICT) {
                state = "Conflict";
                style = "scm-submodule-conflict";
            } else if (submodule->content_untracked) {
                state = "Untracked content";
                style = "scm-submodule-warning";
            } else if (submodule->content_modified) {
                state = "Modified content";
                style = "scm-submodule-modified";
            }
            GitActionContext *context = git_action_context(
                plugin, GIT_UI_SELECT_REPOSITORY, submodule->path, false);
            ca_btn_begin(&(Ca_BtnDesc){
                .direction = CA_HORIZONTAL,
                .style = "scm-submodule-row",
                .on_click = context ? git_on_action : NULL,
                .click_data = context,
                .disabled = plugin->task_running || !context,
            });
            ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_REPO, .style = style });
            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_VERTICAL,
                .style = "scm-submodule-info",
            });
            ca_text(&(Ca_TextDesc){ .text = submodule->path,
                                     .style = "scm-submodule-path" });
            char details[96];
            snprintf(details, sizeof(details), "%.12s  ·  %s", submodule->commit,
                     state);
            ca_text(&(Ca_TextDesc){ .text = details, .style = style });
            ca_div_end();
            ca_btn_end();
            char tooltip[GIT_PATH_CAP + 64u];
            snprintf(tooltip, sizeof(tooltip), "Use %s as the active repository",
                     submodule->path);
            ca_tooltip(&(Ca_TooltipDesc){ .text = tooltip });
        }
        if (plugin->snapshot.omitted_submodule_count > 0u) {
            char omitted[96];
            snprintf(omitted, sizeof(omitted), "%zu additional submodules omitted",
                     plugin->snapshot.omitted_submodule_count);
            ca_text(&(Ca_TextDesc){ .text = omitted, .style = "scm-warning" });
        }
    }
    if (plugin->snapshot.omitted_count > 0u) {
        char omitted[96];
        snprintf(omitted, sizeof(omitted), "%zu additional paths omitted",
                 plugin->snapshot.omitted_count);
        ca_text(&(Ca_TextDesc){ .text = omitted, .style = "scm-warning" });
    }
}

/* Pixel geometry for the commit graph gutter. Lines/dots are plain
 * absolutely-positioned divs (Causality has no line-drawing primitive),
 * matching the technique text_view.c already uses for column rulers —
 * every segment here is axis-aligned (no rotation/diagonal math), which
 * keeps the geometry exact arithmetic instead of guessed pixel values. */
#define GIT_GRAPH_LANE_W 14.0f
#define GIT_GRAPH_LINE_W 2.0f
#define GIT_GRAPH_DOT_SIZE 8.0f
#define GIT_GRAPH_MAX_VISIBLE_LANES 5u

/* Return the highest lane index referenced by any commit's through_lanes
 * bitmask, so the gutter is only as wide as the graph actually needs
 * (capped so a very tangled history can't push the commit text off the
 * edge of the narrow sidebar). */
static unsigned git_graph_lane_span(const GitHistory *history)
{
    uint32_t seen = 0u;
    for (size_t i = 0u; i < history->count; ++i) {
        seen |= history->commits[i].through_lanes;
    }
    unsigned span = 1u;
    for (unsigned lane = 0u; lane < 32u; ++lane) {
        if (seen & (1u << lane)) span = lane + 1u;
    }
    return span > GIT_GRAPH_MAX_VISIBLE_LANES
        ? GIT_GRAPH_MAX_VISIBLE_LANES : span;
}

/* Render one commit row's graph gutter: a full-height vertical line for
 * every lane passing through this row, plus a dot marking this commit's
 * own lane. Lines are drawn full-row-height (not stopping at the dot) so
 * adjacent rows visually connect into continuous vertical rails; the dot
 * simply paints on top, centered in its lane column. */
static void git_render_graph_gutter(const GitCommitEntry *entry,
                                    unsigned lane_span,
                                    float row_height)
{
    const float gutter_w = (float)lane_span * GIT_GRAPH_LANE_W;
    ca_div_begin(&(Ca_DivDesc){
        .width = gutter_w,
        .height = row_height,
        .style = "scm-graph-gutter",
    });
    for (unsigned lane = 0u; lane < lane_span; ++lane) {
        if (!(entry->through_lanes & (1u << lane))) continue;
        const float center_x = ((float)lane + 0.5f) * GIT_GRAPH_LANE_W;
        ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE,
            .pos_x = center_x - GIT_GRAPH_LINE_W * 0.5f,
            .pos_y = 0.0f,
            .width = GIT_GRAPH_LINE_W,
            .height = row_height,
            .style = (int)lane == entry->lane
                ? "scm-graph-line scm-graph-line-active" : "scm-graph-line",
        });
        ca_div_end();
    }
    for (size_t parent = 1u; parent < entry->parent_count; ++parent) {
        const int parent_lane = entry->parent_lanes[parent];
        if (parent_lane < 0 || (unsigned)parent_lane >= lane_span ||
            entry->lane < 0 || (unsigned)entry->lane >= lane_span) continue;
        const float from_x = ((float)entry->lane + 0.5f) * GIT_GRAPH_LANE_W;
        const float to_x = ((float)parent_lane + 0.5f) * GIT_GRAPH_LANE_W;
        const float from_y = row_height * 0.5f;
        const float to_y = row_height;
        const float dx = to_x - from_x;
        const float dy = to_y - from_y;
        const float length = sqrtf(dx * dx + dy * dy);
        if (length <= 0.0f) continue;
        Ca_Div *connector = ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE,
            .pos_x = (from_x + to_x - length) * 0.5f,
            .pos_y = (from_y + to_y - GIT_GRAPH_LINE_W) * 0.5f,
            .width = length,
            .height = GIT_GRAPH_LINE_W,
            .style = "scm-graph-connector",
        });
        ca_div_set_transform(connector, atan2f(dy, dx) * 57.2957795f,
                             1.0f, 1.0f, 0.5f, 0.5f);
        ca_div_end();
    }
    if ((unsigned)entry->lane < lane_span) {
        const float center_x = ((float)entry->lane + 0.5f) * GIT_GRAPH_LANE_W;
        ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE,
            .pos_x = center_x - GIT_GRAPH_DOT_SIZE * 0.5f,
            .pos_y = (row_height - GIT_GRAPH_DOT_SIZE) * 0.5f,
            .width = GIT_GRAPH_DOT_SIZE,
            .height = GIT_GRAPH_DOT_SIZE,
            .corner_radius = GIT_GRAPH_DOT_SIZE * 0.5f,
            .style = entry->parent_count > 1u
                ? "scm-graph-dot scm-graph-dot-merge" : "scm-graph-dot",
        });
        ca_div_end();
    }
    ca_div_end();   /* scm-graph-gutter */
}

/* Render recent repository history as a compact branch/merge graph: a
 * lane gutter (computed by git_model_layout_graph) to the left of each
 * commit's subject/meta text. */
static void git_render_history(GitPlugin *plugin)
{
    if (plugin->history.count == 0u && !plugin->task_running) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "scm-clean-state" });
        ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_GIT_COMMIT, .style = "scm-clean-icon" });
        ca_text(&(Ca_TextDesc){ .text = "No commits found", .style = "scm-empty" });
        ca_div_end();
        return;
    }
    const unsigned lane_span = git_graph_lane_span(&plugin->history);
    const float row_height = 46.0f;
    for (size_t i = 0u; i < plugin->history.count; ++i) {
        const GitCommitEntry *entry = &plugin->history.commits[i];
        GitActionContext *context = git_action_context(
            plugin, GIT_UI_SHOW_COMMIT, entry->hash, false);
        ca_btn_begin(&(Ca_BtnDesc){
            .on_click = context ? git_on_action : NULL,
            .click_data = context,
            .direction = CA_HORIZONTAL,
            .style = "scm-commit-row",
            .disabled = plugin->task_running || !context,
        });
        git_render_graph_gutter(entry, lane_span, row_height);
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "scm-commit-info",
        });
        ca_text(&(Ca_TextDesc){ .text = entry->subject, .style = "scm-commit-subject" });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "scm-commit-meta-row" });
        ca_text(&(Ca_TextDesc){ .text = entry->short_hash, .style = "scm-commit-hash" });
        char metadata[300];
        snprintf(metadata, sizeof(metadata), "%s  \xc2\xb7  %s", entry->author, entry->date);
        ca_text(&(Ca_TextDesc){ .text = metadata, .style = "scm-commit-meta" });
        ca_div_end();
        ca_div_end();   /* scm-commit-info */
        ca_btn_end();
    }
}

/* Render branch creation and local branch checkout controls. */
static void git_render_branches(GitPlugin *plugin)
{
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-branch-create",
    });
    plugin->branch_input = ca_input(&(Ca_InputDesc){
        .text = plugin->new_branch,
        .placeholder = "New branch name",
        .on_change = git_on_branch_change,
        .change_data = plugin,
        .style = "scm-branch-input",
        .disabled = plugin->task_running,
    });
    git_render_button(plugin, "Create", GIT_UI_CREATE_BRANCH, NULL, false,
                      plugin->task_running || !git_has_content(plugin->new_branch),
                      "scm-primary-action");
    ca_div_end();

    if (plugin->branches.count == 0u && !plugin->task_running) {
        ca_text(&(Ca_TextDesc){ .text = "No local branches", .style = "scm-empty" });
    }

    for (size_t i = 0u; i < plugin->branches.count; ++i) {
        const GitBranchEntry *entry = &plugin->branches.branches[i];
        GitActionContext *context = git_action_context(
            plugin, GIT_UI_CHECKOUT, entry->name, false);
        ca_btn_begin(&(Ca_BtnDesc){
            .on_click = context && !entry->current ? git_on_action : NULL,
            .click_data = context,
            .direction = CA_HORIZONTAL,
            .style = entry->current ? "scm-branch-row-current" : "scm-branch-row",
            .disabled = plugin->task_running || entry->current || !context,
        });
        ca_text(&(Ca_TextDesc){
            .text = entry->current ? CA_ICON_NF_COD_CHECK : "",
            .style = "scm-branch-current-icon",
        });
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "scm-branch-info" });
        ca_text(&(Ca_TextDesc){ .text = entry->name, .style = "scm-branch-name" });
        if (entry->upstream[0]) {
            ca_text(&(Ca_TextDesc){ .text = entry->upstream, .style = "scm-branch-upstream" });
        }
        ca_div_end();
        ca_btn_end();
    }
}

/* Render the complete source-control side panel. */
static void git_panel_render(void *user_data)
{
    GitPlugin *plugin = (GitPlugin *)user_data;
    if (!plugin) return;
    plugin->action_count = 0u;
    plugin->commit_input = NULL;
    plugin->branch_input = NULL;

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-header",
    });
    ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_SOURCE_CONTROL, .style = "scm-title-icon" });
    ca_text(&(Ca_TextDesc){ .text = "Source Control", .style = "scm-title" });
    /* Fixed-size slot, always present, never toggled with `hidden` —
     * only its glyph/color/tooltip change with task_running. This is
     * what keeps busy/idle transitions from ever reflowing the rest of
     * the panel (a conditionally-rendered banner used to do that). */
    ca_div_begin(&(Ca_DivDesc){
        .style = plugin->task_running ? "scm-busy-slot scm-busy-slot-active" : "scm-busy-slot",
    });
    if (plugin->task_running) {
        ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_SYNC, .style = "scm-busy-icon" });
    }
    ca_div_end();
    if (plugin->task_running && plugin->activity[0]) {
        ca_tooltip(&(Ca_TooltipDesc){ .text = plugin->activity });
    }
    git_render_icon_button(plugin, CA_ICON_NF_FA_REFRESH, "Refresh",
                           GIT_UI_REFRESH, NULL, false,
                           plugin->task_running || !plugin->snapshot.repository,
                           "scm-header-icon-action");
    git_render_icon_button(plugin, CA_ICON_NF_COD_CLOSE, "Close",
                           GIT_UI_CLOSE, NULL, false, false,
                           "scm-header-icon-action");
    ca_div_end();

    if (plugin->error[0]) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "scm-error" });
        ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_DISCARD, .style = "scm-error-icon" });
        ca_text(&(Ca_TextDesc){ .text = plugin->error, .style = "scm-error-text" });
        ca_div_end();
    }

    if (!plugin->snapshot.repository) {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "scm-no-repository",
        });
        ca_text(&(Ca_TextDesc){ .text = CA_ICON_NF_COD_SOURCE_CONTROL, .style = "scm-clean-icon" });
        ca_text(&(Ca_TextDesc){
            .text = plugin->workspace_root[0]
                ? "The open workspace is not a Git repository."
                : "Open a folder to use source control.",
            .style = "scm-empty",
        });
        if (plugin->workspace_root[0]) {
            git_render_button(plugin, "Initialize Repository", GIT_UI_INIT,
                              NULL, false, plugin->task_running,
                              "scm-primary-action");
        }
        ca_div_end();
        return;
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "scm-repository",
    });
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "scm-branch-row-summary" });
    ca_text(&(Ca_TextDesc){
        .text = plugin->snapshot.detached ? CA_ICON_NF_COD_GIT_COMMIT : CA_ICON_NF_COD_SOURCE_CONTROL,
        .style = "scm-branch-icon",
    });
    ca_text(&(Ca_TextDesc){
        .text = plugin->snapshot.detached ? "detached HEAD" : plugin->snapshot.branch,
        .style = "scm-branch-heading",
    });
    ca_div_end();
    ca_text(&(Ca_TextDesc){ .text = plugin->snapshot.root,
                             .style = "scm-repository-path" });
    ca_tooltip(&(Ca_TooltipDesc){ .text = plugin->snapshot.root });
    if (strcmp(plugin->snapshot.root, plugin->workspace_root) != 0) {
        git_render_button(plugin, "Workspace Repository",
                          GIT_UI_SELECT_WORKSPACE_REPOSITORY, NULL, false,
                          plugin->task_running, "scm-section-action");
    }
    if (plugin->snapshot.upstream[0]) {
        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .style = "scm-upstream-row" });
        ca_text(&(Ca_TextDesc){ .text = plugin->snapshot.upstream, .style = "scm-upstream" });
        if (plugin->snapshot.behind > 0) {
            char behind[24];
            snprintf(behind, sizeof(behind), "%s%d",
                     CA_ICON_NF_COD_ARROW_DOWN, plugin->snapshot.behind);
            ca_text(&(Ca_TextDesc){ .text = behind, .style = "scm-sync-behind" });
        }
        if (plugin->snapshot.ahead > 0) {
            char ahead[24];
            snprintf(ahead, sizeof(ahead), "%s%d",
                     CA_ICON_NF_COD_ARROW_UP, plugin->snapshot.ahead);
            ca_text(&(Ca_TextDesc){ .text = ahead, .style = "scm-sync-ahead" });
        }
        ca_div_end();
    } else {
        ca_text(&(Ca_TextDesc){ .text = "No upstream configured", .style = "scm-upstream" });
    }
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-remote-actions",
    });
    git_render_icon_button(plugin, CA_ICON_NF_COD_GIT_FETCH, "Fetch",
                           GIT_UI_FETCH, NULL, false,
                           plugin->task_running, "scm-action-icon");
    git_render_icon_button(plugin, CA_ICON_NF_COD_CLOUD_DOWNLOAD, "Pull",
                           GIT_UI_PULL, NULL, false,
                           plugin->task_running || !plugin->snapshot.upstream[0],
                           "scm-action-icon");
    git_render_icon_button(plugin, CA_ICON_NF_COD_CLOUD_UPLOAD, "Push",
                           GIT_UI_PUSH, NULL, false,
                           plugin->task_running || plugin->snapshot.detached,
                           "scm-action-icon");
    ca_div_end();

    /* Tabs share the row width evenly (flex-grow, centered) instead of a
     * fixed-padding row — three text labels at a fixed width could overflow
     * a narrow sidebar and clip the last tab. All three tabs use the exact
     * same git_render_button() path (plain Ca_BtnDesc.text, no children) —
     * an earlier version special-cased Changes with an extra dot-badge
     * child so it needed its own manually-styled child ca_text instead of
     * Ca_BtnDesc.text, and that visibly diverged from History/Branches
     * (wrong size/position) for reasons that didn't resolve under
     * investigation. Keeping all three on one identical, simple path
     * removes the divergence outright instead of chasing it further. */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-tabs",
    });
    git_render_button(plugin, "Changes", GIT_UI_CHANGES, NULL, false, false,
                      plugin->tab == GIT_PANEL_CHANGES ? "scm-tab-active" : "scm-tab");
    git_render_button(plugin, "History", GIT_UI_HISTORY, NULL, false, false,
                      plugin->tab == GIT_PANEL_HISTORY ? "scm-tab-active" : "scm-tab");
    git_render_button(plugin, "Branches", GIT_UI_BRANCHES, NULL, false, false,
                      plugin->tab == GIT_PANEL_BRANCHES ? "scm-tab-active" : "scm-tab");
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "scm-content",
        .id = "scm-content-scroll",
    });
    switch (plugin->tab) {
        case GIT_PANEL_CHANGES: git_render_changes(plugin); break;
        case GIT_PANEL_HISTORY: git_render_history(plugin); break;
        case GIT_PANEL_BRANCHES: git_render_branches(plugin); break;
    }
    ca_div_end();
}

/* Convert an absolute active path to a repository-relative path. */
static const char *git_relative_active_path(const GitPlugin *plugin)
{
    if (!plugin || !plugin->snapshot.repository || !plugin->active_file[0]) return NULL;
    const size_t root_length = strlen(plugin->snapshot.root);
    if (root_length == 0u) return NULL;
    if (strncmp(plugin->active_file, plugin->snapshot.root, root_length) != 0) return NULL;
    const char *relative = plugin->active_file + root_length;
    if (*relative == '/' || *relative == '\\') ++relative;
    return *relative ? relative : NULL;
}

/* Drive completed-task adoption and keyboard submission. */
static void git_panel_tick(void *user_data)
{
    GitPlugin *plugin = (GitPlugin *)user_data;
    if (!plugin || plugin->shutting_down) return;
    git_consume_task(plugin);

    if (plugin->needs_commit_focus && plugin->commit_input) {
        ca_input_focus(plugin->commit_input);
        plugin->needs_commit_focus = false;
    }
    if (plugin->commit_input &&
        ca_input_key_pressed(plugin->commit_input, SOL_KEY_ENTER) &&
        plugin->snapshot.staged_count > 0u &&
        git_has_content(plugin->commit_message)) {
        (void)git_start_task(plugin, GIT_TASK_COMMIT,
                             plugin->commit_message, false);
    }
    if (plugin->branch_input &&
        ca_input_key_pressed(plugin->branch_input, SOL_KEY_ENTER) &&
        git_has_content(plugin->new_branch)) {
        (void)git_start_task(plugin, GIT_TASK_CREATE_BRANCH,
                             plugin->new_branch, false);
    }
}

/* Track workspace-root changes and rediscover repository ownership. */
static bool git_on_workspace_root(const SolEvent *event, void *user_data)
{
    GitPlugin *plugin = (GitPlugin *)user_data;
    const SolFileTreeRootPayload *payload = event && event->payload
        ? (const SolFileTreeRootPayload *)event->payload : NULL;
    if (!plugin || plugin->shutting_down) return false;
    git_copy_string(plugin->workspace_root, sizeof(plugin->workspace_root),
                    payload ? payload->path : NULL);
    memset(&plugin->snapshot, 0, sizeof(plugin->snapshot));
    memset(&plugin->history, 0, sizeof(plugin->history));
    memset(&plugin->branches, 0, sizeof(plugin->branches));
    plugin->pending_discard[0] = '\0';
    plugin->rediscover_pending = plugin->task_running;
    if (!plugin->task_running && plugin->workspace_root[0]) {
        plugin->rediscover_pending = false;
        (void)git_start_task(plugin, GIT_TASK_DISCOVER, NULL, false);
    }
    git_update_status_segment(plugin);
    sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
    return false;
}

/* Track the focused file for diff and blame commands. */
static bool git_on_buffer_focused(const SolEvent *event, void *user_data)
{
    GitPlugin *plugin = (GitPlugin *)user_data;
    const SolBufferEventPayload *payload = event && event->payload
        ? (const SolBufferEventPayload *)event->payload : NULL;
    if (!plugin) return false;
    git_copy_string(plugin->active_file, sizeof(plugin->active_file),
                    payload ? payload->source_path : NULL);
    return false;
}

/* Dispatch command-palette and leader-chord Git actions. */
static bool git_on_command(const char *action,
                           const SolInputEvent *event,
                           void *user_data)
{
    (void)event;
    GitPlugin *plugin = (GitPlugin *)user_data;
    if (!plugin || !action) return false;
    if (strcmp(action, "git.status") == 0) {
        if (sol_plugin_side_panel_visible(plugin->ctx, plugin->panel_token)) {
            sol_plugin_hide_side_panel(plugin->ctx, plugin->panel_token);
        } else {
            (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
            plugin->tab = GIT_PANEL_CHANGES;
            if (!plugin->task_running && plugin->snapshot.repository) {
                (void)git_start_task(plugin, GIT_TASK_REFRESH, NULL, false);
            }
        }
        return true;
    }
    if (strcmp(action, "git.refresh") == 0) {
        (void)git_start_task(plugin, GIT_TASK_REFRESH, NULL, false);
        return true;
    }
    if (strcmp(action, "git.commit") == 0) {
        (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
        plugin->tab = GIT_PANEL_CHANGES;
        plugin->needs_commit_focus = true;
        sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
        return true;
    }
    if (strcmp(action, "git.history") == 0) {
        (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
        plugin->tab = GIT_PANEL_HISTORY;
        if (!plugin->task_running) {
            (void)git_start_task(plugin, GIT_TASK_HISTORY, NULL, false);
        }
        return true;
    }
    if (strcmp(action, "git.branches") == 0) {
        (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
        plugin->tab = GIT_PANEL_BRANCHES;
        if (!plugin->task_running) {
            (void)git_start_task(plugin, GIT_TASK_BRANCHES, NULL, false);
        }
        return true;
    }
    if (strcmp(action, "git.diff") == 0) {
        const char *relative = git_relative_active_path(plugin);
        if (!relative) {
            git_copy_string(plugin->error, sizeof(plugin->error),
                            "The active file is outside this repository");
            (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            return true;
        }
        (void)git_start_task(plugin, GIT_TASK_DIFF, relative, false);
        return true;
    }
    if (strcmp(action, "git.blame") == 0) {
        const char *relative = git_relative_active_path(plugin);
        if (!relative) {
            git_copy_string(plugin->error, sizeof(plugin->error),
                            "The active file is outside this repository");
            (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
            sol_plugin_notify_side_panel(plugin->ctx, plugin->panel_token);
            return true;
        }
        (void)git_start_task(plugin, GIT_TASK_BLAME, relative, false);
        return true;
    }
    if (strcmp(action, "git.fetch") == 0) {
        (void)git_start_task(plugin, GIT_TASK_FETCH, NULL, false);
        return true;
    }
    if (strcmp(action, "git.pull") == 0) {
        (void)git_start_task(plugin, GIT_TASK_PULL, NULL, false);
        return true;
    }
    if (strcmp(action, "git.push") == 0) {
        (void)git_start_task(plugin, GIT_TASK_PUSH, NULL, false);
        return true;
    }
    return false;
}

/* Register one leader-chord command. */
static bool git_register_command(GitPlugin *plugin,
                                 const char *action,
                                 SolKeyCode first,
                                 SolKeyCode second)
{
    const SolKeyCode chord[] = { first, second };
    return sol_plugin_register_command(
        plugin->ctx,
        &(SolPluginCommandDesc){
            .action = action,
            .label = action,
            .chord = chord,
            .chord_length = 2u,
            .callback = git_on_command,
            .user_data = plugin,
        });
}

/* Register mouse-accessible menu entries for Git commands. */
static bool git_register_menu_items(GitPlugin *plugin)
{
    if (!plugin || !plugin->ctx) return false;
    const SolPluginMenuItemDesc items[] = {
        {
            .menu_id = "view", .menu_label = "View",
            .item_id = "git-source-control", .label = "Source Control",
            .action = "git.status", .menu_order = 400, .item_order = 1100,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-status", .label = "Source Control",
            .action = "git.status", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1100,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-refresh", .label = "Refresh",
            .action = "git.refresh", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1110,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-diff", .label = "Diff Active File",
            .action = "git.diff", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1120,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-history", .label = "History",
            .action = "git.history", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1130,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-branches", .label = "Branches",
            .action = "git.branches", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1140,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-blame", .label = "Blame Active File",
            .action = "git.blame", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1150,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-commit", .label = "Commit",
            .action = "git.commit", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1160,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-fetch", .label = "Fetch",
            .action = "git.fetch", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1170,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-pull", .label = "Pull",
            .action = "git.pull", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1180,
        },
        {
            .menu_id = "plugins", .menu_label = "Plugins",
            .item_id = "git-push", .label = "Push",
            .action = "git.push", .submenu_id = "git", .submenu_label = "Git",
            .menu_order = 900, .item_order = 1190,
        },
    };
    for (size_t i = 0u; i < sizeof(items) / sizeof(items[0]); ++i) {
        if (sol_plugin_register_menu_item(plugin->ctx, &items[i]) ==
            SOL_PLUGIN_MENU_ITEM_TOKEN_INVALID) {
            return false;
        }
    }
    return true;
}

/* Free plugin state after automatic UI and service cleanup. */
static void git_plugin_destroy(void *service, void *user_data)
{
    (void)user_data;
    GitPlugin *plugin = (GitPlugin *)service;
    if (!plugin) return;
    plugin->shutting_down = true;
    if (plugin->task_running) sol_job_fence_wait(plugin->task_fence);
    if (plugin->task) {
        free(plugin->task->output);
        free(plugin->task);
    }
    sol_job_fence_destroy(plugin->task_fence);
    free(plugin);
}

/* Initialize Git integration and register all UI, command, and event resources. */
static bool git_on_load(SolPluginCtx *ctx)
{
    GitPlugin *plugin = (GitPlugin *)calloc(1u, sizeof(*plugin));
    if (!plugin) return false;
    plugin->ctx = ctx;
    plugin->tab = GIT_PANEL_CHANGES;
    plugin->task_fence = sol_job_fence_create();
    atomic_init(&plugin->task_done, false);
    if (!plugin->task_fence) {
        free(plugin);
        return false;
    }

    if (!sol_plugin_register_service(ctx, "git.plugin.state", 1u, plugin,
                                     git_plugin_destroy, NULL)) {
        sol_job_fence_destroy(plugin->task_fence);
        free(plugin);
        return false;
    }

    plugin->panel_token = sol_plugin_register_side_panel(
        ctx,
        &(SolPluginSidePanelDesc){
            .id = "git.source_control",
            .title = "Source Control",
            .render = git_panel_render,
            .tick = git_panel_tick,
            .user_data = plugin,
        });
    if (plugin->panel_token == SOL_PLUGIN_SIDE_PANEL_TOKEN_INVALID) return false;

    plugin->status_token = sol_plugin_add_status_segment(
        ctx, "git: no repository", "status-plugin");
    if (plugin->status_token == SOL_PLUGIN_STATUS_TOKEN_INVALID) return false;

    if (!git_register_command(plugin, "git.status", 'G', 'G') ||
        !git_register_command(plugin, "git.refresh", 'G', 'R') ||
        !git_register_command(plugin, "git.diff", 'G', 'D') ||
        !git_register_command(plugin, "git.history", 'G', 'L') ||
        !git_register_command(plugin, "git.branches", 'G', 'H') ||
        !git_register_command(plugin, "git.blame", 'G', 'B') ||
        !git_register_command(plugin, "git.commit", 'G', 'C') ||
        !git_register_command(plugin, "git.fetch", 'G', 'F') ||
        !git_register_command(plugin, "git.pull", 'G', 'U') ||
        !git_register_command(plugin, "git.push", 'G', 'P')) {
        return false;
    }
    if (!git_register_menu_items(plugin)) return false;

    plugin->root_subscription = sol_plugin_subscribe(
        ctx, SOL_EVENT_FILE_TREE_ROOT, git_on_workspace_root, plugin);
    plugin->focus_subscription = sol_plugin_subscribe(
        ctx, SOL_EVENT_BUFFER_FOCUSED, git_on_buffer_focused, plugin);
    if (!plugin->root_subscription || !plugin->focus_subscription) return false;

    SolUISystem *ui = sol_plugin_ui(ctx);
    git_copy_string(plugin->workspace_root, sizeof(plugin->workspace_root),
                    ui ? sol_ui_system_file_tree_root(ui) : NULL);
    SolBufferId active = sol_plugin_active_buffer(ctx);
    SolBuffer *buffer = sol_buffer_get(sol_plugin_buffers(ctx), active);
    SolTextBuffer *text = buffer ? sol_text_buffer_state(buffer) : NULL;
    git_copy_string(plugin->active_file, sizeof(plugin->active_file),
                    text ? sol_text_buffer_source_path(text) : NULL);

    if (plugin->workspace_root[0]) {
        (void)git_start_task(plugin, GIT_TASK_DISCOVER, NULL, false);
    }
    return true;
}

/* Stop accepting work and wait until the active Git task is quiescent. */
static void git_on_unload(SolPluginCtx *ctx)
{
    GitPlugin *plugin = (GitPlugin *)sol_plugin_get_service(
        ctx, "git.plugin.state", 1u);
    if (!plugin) return;
    plugin->shutting_down = true;
    if (plugin->task_running) sol_job_fence_wait(plugin->task_fence);
}

static const SolPluginAPI g_git_api = {
    .api_version = SOL_PLUGIN_API_VERSION,
    .id = "com.sol.plugin.git",
    .display_name = "Git Source Control",
    .version = "2.0.0",
    .on_load = git_on_load,
    .on_unload = git_on_unload,
};

/* Export the versioned plugin descriptor to Sol's dynamic loader. */
bool sol_plugin_query(uint32_t requested_api_version, SolPluginAPI *out_api)
{
    if (requested_api_version != SOL_PLUGIN_API_VERSION || !out_api) return false;
    *out_api = g_git_api;
    return true;
}
