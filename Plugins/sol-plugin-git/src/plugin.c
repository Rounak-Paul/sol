#include "git_plugin.h"

#include <causality.h>

#include "sol_plugin.h"
#include "sol_text_buffer.h"
#include "sol_ui_system.h"

#include <stdatomic.h>
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
            success = git_model_discover(task->workspace_root,
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
            case GIT_TASK_DISCOVER:
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

/* Render one changed-file row with open, diff, and mutation actions. */
static void git_render_file_row(GitPlugin *plugin,
                                const GitFileStatus *file,
                                bool staged)
{
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-file-row",
    });
    ca_text(&(Ca_TextDesc){
        .text = git_file_status_label(file, staged),
        .style = git_file_status_style(file, staged),
    });
    git_render_button(plugin, file->path, GIT_UI_OPEN, file->path, false,
                      false, "scm-file-open");
    git_render_button(plugin, "Diff", GIT_UI_DIFF, file->path, staged,
                      plugin->task_running, "scm-row-action");
    if (staged) {
        git_render_button(plugin, "-", GIT_UI_UNSTAGE, file->path, false,
                          plugin->task_running, "scm-row-action");
    } else {
        git_render_button(plugin, "+", GIT_UI_STAGE, file->path, false,
                          plugin->task_running, "scm-row-action");
        git_render_button(plugin, "x", GIT_UI_DISCARD, file->path,
                          file->kind == GIT_FILE_UNTRACKED,
                          plugin->task_running, "scm-row-danger");
    }
    ca_div_end();
}

/* Render a staged or unstaged file group. */
static void git_render_file_group(GitPlugin *plugin,
                                  const char *title,
                                  bool staged)
{
    const size_t count = staged ? plugin->snapshot.staged_count
                                : plugin->snapshot.unstaged_count;
    char heading[128];
    snprintf(heading, sizeof(heading), "%s  %zu", title, count);
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-section-header",
    });
    ca_text(&(Ca_TextDesc){ .text = heading, .style = "scm-section-title" });
    if (count > 0u) {
        git_render_button(plugin, staged ? "Unstage All" : "Stage All",
                          staged ? GIT_UI_UNSTAGE_ALL : GIT_UI_STAGE_ALL,
                          NULL, false, plugin->task_running, "scm-section-action");
    }
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
        .placeholder = "Commit message",
        .on_change = git_on_commit_change,
        .change_data = plugin,
        .style = "scm-commit-input",
        .disabled = plugin->task_running,
    });
    git_render_button(plugin, "Commit", GIT_UI_COMMIT, NULL, false,
                      plugin->task_running || plugin->snapshot.staged_count == 0u ||
                      !git_has_content(plugin->commit_message),
                      "scm-primary-action");
    ca_div_end();

    if (plugin->pending_discard[0]) {
        char message[GIT_PATH_CAP + 96u];
        snprintf(message, sizeof(message), "Discard local changes to %s?",
                 plugin->pending_discard);
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

    if (plugin->snapshot.file_count == 0u) {
        ca_text(&(Ca_TextDesc){
            .text = "Working tree clean",
            .style = "scm-empty",
        });
        return;
    }
    git_render_file_group(plugin, "STAGED CHANGES", true);
    git_render_file_group(plugin, "CHANGES", false);
    if (plugin->snapshot.omitted_count > 0u) {
        char omitted[96];
        snprintf(omitted, sizeof(omitted), "%zu additional paths omitted",
                 plugin->snapshot.omitted_count);
        ca_text(&(Ca_TextDesc){ .text = omitted, .style = "scm-warning" });
    }
}

/* Render recent repository history. */
static void git_render_history(GitPlugin *plugin)
{
    if (plugin->history.count == 0u && !plugin->task_running) {
        ca_text(&(Ca_TextDesc){ .text = "No commits found", .style = "scm-empty" });
        return;
    }
    for (size_t i = 0u; i < plugin->history.count; ++i) {
        const GitCommitEntry *entry = &plugin->history.commits[i];
        GitActionContext *context = git_action_context(
            plugin, GIT_UI_SHOW_COMMIT, entry->hash, false);
        ca_btn_begin(&(Ca_BtnDesc){
            .on_click = context ? git_on_action : NULL,
            .click_data = context,
            .direction = CA_VERTICAL,
            .style = "scm-commit-row",
            .disabled = plugin->task_running || !context,
        });
        ca_text(&(Ca_TextDesc){ .text = entry->subject, .style = "scm-commit-subject" });
        char metadata[320];
        snprintf(metadata, sizeof(metadata), "%s  %s  %s",
                 entry->short_hash, entry->author, entry->date);
        ca_text(&(Ca_TextDesc){ .text = metadata, .style = "scm-commit-meta" });
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

    for (size_t i = 0u; i < plugin->branches.count; ++i) {
        const GitBranchEntry *entry = &plugin->branches.branches[i];
        GitActionContext *context = git_action_context(
            plugin, GIT_UI_CHECKOUT, entry->name, false);
        ca_btn_begin(&(Ca_BtnDesc){
            .on_click = context && !entry->current ? git_on_action : NULL,
            .click_data = context,
            .direction = CA_VERTICAL,
            .style = entry->current ? "scm-branch-row-current" : "scm-branch-row",
            .disabled = plugin->task_running || entry->current || !context,
        });
        ca_text(&(Ca_TextDesc){ .text = entry->name, .style = "scm-branch-name" });
        if (entry->upstream[0]) {
            ca_text(&(Ca_TextDesc){ .text = entry->upstream, .style = "scm-branch-upstream" });
        }
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
    ca_text(&(Ca_TextDesc){ .text = "SOURCE CONTROL", .style = "scm-title" });
    git_render_button(plugin, "Refresh", GIT_UI_REFRESH, NULL, false,
                      plugin->task_running || !plugin->snapshot.repository,
                      "scm-header-action");
    git_render_button(plugin, "Close", GIT_UI_CLOSE, NULL, false,
                      false, "scm-header-action");
    ca_div_end();

    if (plugin->error[0]) {
        ca_text(&(Ca_TextDesc){ .text = plugin->error, .style = "scm-error" });
    }
    if (plugin->task_running && plugin->activity[0]) {
        ca_text(&(Ca_TextDesc){ .text = plugin->activity, .style = "scm-activity" });
    }

    if (!plugin->snapshot.repository) {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "scm-no-repository",
        });
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
    ca_text(&(Ca_TextDesc){
        .text = plugin->snapshot.detached ? "detached HEAD" : plugin->snapshot.branch,
        .style = "scm-branch-heading",
    });
    char sync[256];
    if (plugin->snapshot.upstream[0]) {
        snprintf(sync, sizeof(sync), "%s   up %d   down %d",
                 plugin->snapshot.upstream,
                 plugin->snapshot.ahead,
                 plugin->snapshot.behind);
    } else {
        snprintf(sync, sizeof(sync), "No upstream configured");
    }
    ca_text(&(Ca_TextDesc){ .text = sync, .style = "scm-upstream" });
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "scm-remote-actions",
    });
    git_render_button(plugin, "Fetch", GIT_UI_FETCH, NULL, false,
                      plugin->task_running, "scm-action");
    git_render_button(plugin, "Pull", GIT_UI_PULL, NULL, false,
                      plugin->task_running || !plugin->snapshot.upstream[0],
                      "scm-action");
    git_render_button(plugin, "Push", GIT_UI_PUSH, NULL, false,
                      plugin->task_running || plugin->snapshot.detached,
                      "scm-action");
    ca_div_end();

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
        (void)sol_plugin_show_side_panel(plugin->ctx, plugin->panel_token);
        plugin->tab = GIT_PANEL_CHANGES;
        if (!plugin->task_running && plugin->snapshot.repository) {
            (void)git_start_task(plugin, GIT_TASK_REFRESH, NULL, false);
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

    if (!git_register_command(plugin, "git.status", 'G', 'S') ||
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
