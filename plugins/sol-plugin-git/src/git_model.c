#include "git_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy a bounded byte span into a NUL-terminated destination. */
static void git_copy_span(char *destination,
                          size_t capacity,
                          const char *source,
                          size_t length)
{
    if (!destination || capacity == 0u) return;
    const size_t copy = length < capacity - 1u ? length : capacity - 1u;
    if (copy > 0u && source) memcpy(destination, source, copy);
    destination[copy] = '\0';
}

/* Return true when path is absolute on the host platform. */
static bool git_path_is_absolute(const char *path)
{
#if defined(_WIN32)
    return path[0] == '/' || path[0] == '\\' ||
           (path[0] != '\0' && path[1] == ':');
#else
    return path[0] == '/';
#endif
}

/* Store a concise command failure message. */
static void git_command_error(char *error,
                              size_t capacity,
                              const char *operation,
                              const GitProcessResult *result,
                              const char *output)
{
    if (!error || capacity == 0u) return;
    if (result && result->timed_out) {
        snprintf(error, capacity, "%s timed out", operation);
        return;
    }
    if (output && output[0]) {
        size_t length = strcspn(output, "\r\n");
        snprintf(error, capacity, "%s failed: %.*s", operation,
                 (int)length, output);
    } else {
        snprintf(error, capacity, "%s failed with exit code %d", operation,
                 result ? result->exit_code : -1);
    }
}

/* Return the payload after field_count space-delimited fields. */
static const char *git_record_payload(const char *record,
                                      size_t length,
                                      size_t field_count)
{
    size_t fields = 0u;
    for (size_t i = 0u; i < length; ++i) {
        if (record[i] == ' ') {
            ++fields;
            if (fields == field_count) return record + i + 1u;
        }
    }
    return NULL;
}

/* Append one parsed file record and update aggregate counters. */
static void git_append_file(GitSnapshot *snapshot,
                            GitFileKind kind,
                            char index_status,
                            char worktree_status,
                            const char *path,
                            size_t path_length,
                            const char *original,
                            size_t original_length)
{
    if (snapshot->file_count >= GIT_MAX_FILES) {
        ++snapshot->omitted_count;
        return;
    }
    GitFileStatus *file = &snapshot->files[snapshot->file_count++];
    memset(file, 0, sizeof(*file));
    file->kind = kind;
    file->index_status = index_status;
    file->worktree_status = worktree_status;
    git_copy_span(file->path, sizeof(file->path), path, path_length);
    git_copy_span(file->original_path, sizeof(file->original_path),
                  original, original_length);

    if (kind == GIT_FILE_UNTRACKED) {
        ++snapshot->untracked_count;
        ++snapshot->unstaged_count;
    } else {
        if (index_status != '.' && index_status != ' ') ++snapshot->staged_count;
        if (worktree_status != '.' && worktree_status != ' ') ++snapshot->unstaged_count;
        if (kind == GIT_FILE_UNMERGED &&
            index_status == '.' && worktree_status == '.') {
            ++snapshot->unstaged_count;
        }
    }
}

/* Read the porcelain-v2 submodule summary carried by a tracked path record. */
static void git_apply_file_submodule_state(GitFileStatus *file,
                                           const char *field,
                                           size_t length)
{
    if (!file || !field || length < 4u || field[0] != 'S') return;
    file->submodule = true;
    file->submodule_modified = field[2] == 'M';
    file->submodule_untracked = field[3] == 'U';
}

/* Parse one porcelain-v2 record. */
static bool git_parse_status_record(const char *record,
                                    size_t length,
                                    const char *next_record,
                                    size_t next_length,
                                    bool *consume_next,
                                    GitSnapshot *snapshot)
{
    *consume_next = false;
    if (length == 0u) return true;
    if (record[0] == '#') {
        static const char head_prefix[] = "# branch.head ";
        static const char upstream_prefix[] = "# branch.upstream ";
        static const char ahead_prefix[] = "# branch.ab ";
        if (length > sizeof(head_prefix) - 1u &&
            memcmp(record, head_prefix, sizeof(head_prefix) - 1u) == 0) {
            const char *value = record + sizeof(head_prefix) - 1u;
            const size_t value_length = length - (sizeof(head_prefix) - 1u);
            if (value_length == 10u && memcmp(value, "(detached)", 10u) == 0) {
                snapshot->detached = true;
                git_copy_span(snapshot->branch, sizeof(snapshot->branch),
                              "detached", 8u);
            } else {
                git_copy_span(snapshot->branch, sizeof(snapshot->branch),
                              value, value_length);
            }
        } else if (length > sizeof(upstream_prefix) - 1u &&
                   memcmp(record, upstream_prefix,
                          sizeof(upstream_prefix) - 1u) == 0) {
            git_copy_span(snapshot->upstream, sizeof(snapshot->upstream),
                          record + sizeof(upstream_prefix) - 1u,
                          length - (sizeof(upstream_prefix) - 1u));
        } else if (length > sizeof(ahead_prefix) - 1u &&
                   memcmp(record, ahead_prefix, sizeof(ahead_prefix) - 1u) == 0) {
            (void)sscanf(record + sizeof(ahead_prefix) - 1u,
                         "+%d -%d", &snapshot->ahead, &snapshot->behind);
        }
        return true;
    }

    if (record[0] == '?' && length > 2u && record[1] == ' ') {
        git_append_file(snapshot, GIT_FILE_UNTRACKED, '?', '?',
                        record + 2u, length - 2u, NULL, 0u);
        return true;
    }
    if (record[0] == '!') return true;

    if ((record[0] == '1' || record[0] == '2' || record[0] == 'u') &&
        length > 4u && record[1] == ' ') {
        const char index_status = record[2];
        const char worktree_status = record[3];
        const char *submodule = git_record_payload(record, length, 2u);
        size_t fields = record[0] == '1' ? 8u : (record[0] == '2' ? 9u : 10u);
        const char *path = git_record_payload(record, length, fields);
        if (!path || path > record + length) return false;
        const size_t path_length = (size_t)(record + length - path);
        GitFileKind kind = record[0] == '2' ? GIT_FILE_RENAMED :
                           (record[0] == 'u' ? GIT_FILE_UNMERGED :
                                              GIT_FILE_ORDINARY);
        const char *original = NULL;
        size_t original_length = 0u;
        if (record[0] == '2') {
            original = next_record;
            original_length = next_length;
            *consume_next = true;
        }
        const size_t file_count = snapshot->file_count;
        git_append_file(snapshot, kind, index_status, worktree_status,
                        path, path_length, original, original_length);
        if (snapshot->file_count > file_count) {
            git_apply_file_submodule_state(&snapshot->files[file_count], submodule,
                                           submodule ? strcspn(submodule, " ") : 0u);
        }
        return true;
    }
    return false;
}

/* Parse recursive `git submodule status --cached` lines into the snapshot. */
static bool git_model_parse_submodules(const char *data,
                                       size_t length,
                                       GitSnapshot *snapshot,
                                       char *error,
                                       size_t error_capacity)
{
    if (!data || !snapshot) return false;
    size_t offset = 0u;
    while (offset < length) {
        const char *line = data + offset;
        const char *end = memchr(line, '\n', length - offset);
        if (!end) end = data + length;
        const size_t line_length = (size_t)(end - line);
        if (line_length > 0u) {
            const char *hash_end = memchr(line + 1u, ' ', line_length - 1u);
            const size_t hash_length = hash_end ? (size_t)(hash_end - line - 1u) : 0u;
            if (!hash_end || (hash_length != 40u && hash_length != 64u) ||
                hash_end + 1u == end) {
                if (error && error_capacity > 0u) {
                    snprintf(error, error_capacity, "Malformed Git submodule status");
                }
                return false;
            }
            if (snapshot->submodule_count >= GIT_MAX_SUBMODULES) {
                ++snapshot->omitted_submodule_count;
            } else {
                GitSubmodule *submodule =
                    &snapshot->submodules[snapshot->submodule_count++];
                memset(submodule, 0, sizeof(*submodule));
                git_copy_span(submodule->commit, sizeof(submodule->commit),
                              line + 1u, hash_length);
                const char *path = hash_end + 1u;
                const char *path_end = end;
                for (const char *p = path; p + 2u < end; ++p) {
                    if (p[0] == ' ' && p[1] == '(' && end[-1] == ')') {
                        path_end = p;
                        break;
                    }
                }
                git_copy_span(submodule->path, sizeof(submodule->path), path,
                              (size_t)(path_end - path));
                switch (line[0]) {
                    case '-': submodule->state = GIT_SUBMODULE_UNINITIALIZED; break;
                    case '+': submodule->state = GIT_SUBMODULE_REVISION_CHANGED; break;
                    case 'U': submodule->state = GIT_SUBMODULE_CONFLICT; break;
                    case ' ': submodule->state = GIT_SUBMODULE_CLEAN; break;
                    default:
                        if (error && error_capacity > 0u) {
                            snprintf(error, error_capacity,
                                     "Unsupported Git submodule status '%c'", line[0]);
                        }
                        return false;
                }
                for (size_t i = 0u; i < snapshot->file_count; ++i) {
                    const GitFileStatus *file = &snapshot->files[i];
                    if (!file->submodule || strcmp(file->path, submodule->path) != 0) {
                        continue;
                    }
                    submodule->content_modified = file->submodule_modified;
                    submodule->content_untracked = file->submodule_untracked;
                    break;
                }
            }
        }
        offset = end < data + length ? (size_t)(end - data) + 1u : length;
    }
    return true;
}

/* Load all registered submodules, including clean and nested repositories. */
static bool git_model_refresh_submodules(const char *root,
                                         GitSnapshot *snapshot,
                                         char *error,
                                         size_t error_capacity)
{
    char *output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!output) return false;
    const char *argv[] = {
        "git", "--no-optional-locks", "submodule", "status", "--recursive", "--cached", NULL
    };
    GitProcessResult result = git_process_run(root, argv, output,
                                              GIT_PROCESS_OUTPUT_CAP, 30000u);
    if (result.exit_code != 0 || result.truncated) {
        if (result.truncated && error && error_capacity > 0u) {
            snprintf(error, error_capacity, "Git submodule output exceeded %u bytes",
                     GIT_PROCESS_OUTPUT_CAP - 1u);
        } else {
            git_command_error(error, error_capacity, "Git submodule status", &result,
                              output);
        }
        free(output);
        return false;
    }
    const bool parsed = git_model_parse_submodules(output, result.output_len,
                                                   snapshot, error, error_capacity);
    free(output);
    if (!parsed) return false;
    for (size_t i = 0u; i < snapshot->submodule_count; ++i) {
        if (snapshot->submodules[i].state != GIT_SUBMODULE_UNINITIALIZED) {
            git_model_read_submodule_head(root, &snapshot->submodules[i]);
        }
    }
    return true;
}

/* Read the first line of a small text file without its line terminator. */
static bool git_read_first_line(const char *path, char *out, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool read = fgets(out, (int)capacity, file) != NULL;
    fclose(file);
    if (!read) return false;
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

/* Follow a submodule's .git file or directory to its HEAD; see git_plugin.h. */
void git_model_read_submodule_head(const char *root, GitSubmodule *submodule)
{
    if (!root || !submodule) return;
    submodule->branch[0] = '\0';
    submodule->detached = false;

    char worktree[GIT_PATH_CAP];
    char dot_git[GIT_PATH_CAP];
    char git_dir[GIT_PATH_CAP];
    char line[GIT_PATH_CAP];
    int written = snprintf(worktree, sizeof(worktree), "%s/%s", root, submodule->path);
    if (written <= 0 || (size_t)written >= sizeof(worktree)) return;
    written = snprintf(dot_git, sizeof(dot_git), "%s/.git", worktree);
    if (written <= 0 || (size_t)written >= sizeof(dot_git)) return;

    static const char k_gitdir_prefix[] = "gitdir: ";
    if (git_read_first_line(dot_git, line, sizeof(line)) &&
        strncmp(line, k_gitdir_prefix, sizeof(k_gitdir_prefix) - 1u) == 0) {
        const char *target = line + sizeof(k_gitdir_prefix) - 1u;
        written = git_path_is_absolute(target)
            ? snprintf(git_dir, sizeof(git_dir), "%s", target)
            : snprintf(git_dir, sizeof(git_dir), "%s/%s", worktree, target);
    } else {
        written = snprintf(git_dir, sizeof(git_dir), "%s", dot_git);
    }
    if (written <= 0 || (size_t)written >= sizeof(git_dir)) return;

    char head_path[GIT_PATH_CAP];
    written = snprintf(head_path, sizeof(head_path), "%s/HEAD", git_dir);
    if (written <= 0 || (size_t)written >= sizeof(head_path)) return;
    if (!git_read_first_line(head_path, line, sizeof(line))) return;

    static const char k_branch_prefix[] = "ref: refs/heads/";
    static const char k_ref_prefix[] = "ref: ";
    if (strncmp(line, k_branch_prefix, sizeof(k_branch_prefix) - 1u) == 0) {
        git_copy_span(submodule->branch, sizeof(submodule->branch),
                      line + sizeof(k_branch_prefix) - 1u,
                      strlen(line + sizeof(k_branch_prefix) - 1u));
    } else if (strncmp(line, k_ref_prefix, sizeof(k_ref_prefix) - 1u) != 0 && line[0]) {
        submodule->detached = true;
    }
}

/* Parse porcelain-v2 status bytes into a repository snapshot. */
bool git_model_parse_status(const char *data,
                            size_t length,
                            GitSnapshot *snapshot,
                            char *error,
                            size_t error_capacity)
{
    if (!data || !snapshot) return false;
    char root[GIT_PATH_CAP];
    git_copy_span(root, sizeof(root), snapshot->root, strlen(snapshot->root));
    memset(snapshot, 0, sizeof(*snapshot));
    git_copy_span(snapshot->root, sizeof(snapshot->root), root, strlen(root));
    snapshot->repository = true;

    size_t offset = 0u;
    while (offset < length) {
        while (offset < length && (data[offset] == '\0' || data[offset] == '\n' ||
                                   data[offset] == '\r')) ++offset;
        if (offset >= length) break;
        size_t end = offset;
        while (end < length && data[end] != '\0' && data[end] != '\n') ++end;

        size_t next_offset = end < length ? end + 1u : end;
        while (next_offset < length && (data[next_offset] == '\n' ||
                                        data[next_offset] == '\r')) ++next_offset;
        size_t next_end = next_offset;
        while (next_end < length && data[next_end] != '\0' &&
               data[next_end] != '\n') ++next_end;

        bool consume_next = false;
        if (!git_parse_status_record(data + offset, end - offset,
                                     data + next_offset, next_end - next_offset,
                                     &consume_next, snapshot)) {
            if (error && error_capacity > 0u) {
                snprintf(error, error_capacity,
                         "Unsupported Git status record near byte %zu", offset);
            }
            return false;
        }
        offset = consume_next ? (next_end < length ? next_end + 1u : next_end)
                              : (end < length ? end + 1u : end);
    }
    return true;
}

/* Return the byte span of line index `wanted` (0-based) in text. */
static bool git_output_line(const char *text,
                            size_t wanted,
                            const char **line,
                            size_t *length)
{
    const char *cursor = text;
    for (size_t index = 0u; cursor && *cursor; ++index) {
        const size_t span = strcspn(cursor, "\r\n");
        if (index == wanted) {
            *line = cursor;
            *length = span;
            return span > 0u;
        }
        cursor += span;
        while (*cursor == '\r' || *cursor == '\n') ++cursor;
    }
    return false;
}

/* Resolve an existing path to canonical absolute form; see git_plugin.h. */
bool git_path_canonicalize(const char *path, char *out, size_t capacity)
{
    if (!path || !path[0] || !out || capacity == 0u) return false;
#if defined(_WIN32)
    char *resolved = _fullpath(NULL, path, 0);
#else
    char *resolved = realpath(path, NULL);
#endif
    if (!resolved) return false;
    const size_t length = strlen(resolved);
    const bool fits = length < capacity;
    if (fits) {
        memcpy(out, resolved, length + 1u);
#if defined(_WIN32)
        for (char *c = out; *c; ++c) if (*c == '\\') *c = '/';
#endif
        size_t end = length;
        while (end > 1u && out[end - 1u] == '/' && out[end - 2u] != ':') out[--end] = '\0';
    }
    free(resolved);
    return fits;
}

/* Canonicalize a rev-parse directory that may be relative to cwd. */
static bool git_resolve_git_path(const char *cwd,
                                 const char *line,
                                 size_t length,
                                 char *out,
                                 size_t capacity)
{
    char joined[GIT_PATH_CAP];
    char value[GIT_PATH_CAP];
    git_copy_span(value, sizeof(value), line, length);
    const int written = git_path_is_absolute(value)
        ? snprintf(joined, sizeof(joined), "%s", value)
        : snprintf(joined, sizeof(joined), "%s/%s", cwd, value);
    if (written <= 0 || (size_t)written >= sizeof(joined)) return false;
    return git_path_canonicalize(joined, out, capacity);
}

/* Discover the containing repository and its metadata directories. */
bool git_model_discover(const char *path,
                        char *root,
                        size_t root_capacity,
                        GitRepoPaths *paths,
                        char *error,
                        size_t error_capacity)
{
    if (!path || !path[0] || !root || root_capacity == 0u) return false;
    char *output = (char *)calloc(3u * GIT_PATH_CAP, 1u);
    if (!output) return false;
    const char *argv[] = {
        "git", "rev-parse", "--show-toplevel", "--absolute-git-dir",
        "--git-common-dir", NULL
    };
    GitProcessResult result = git_process_run(path, argv, output,
                                              3u * GIT_PATH_CAP, 15000u);
    if (result.exit_code != 0) {
        git_command_error(error, error_capacity, "Repository discovery", &result, output);
        free(output);
        return false;
    }
    const char *line = NULL;
    size_t length = 0u;
    if (!git_output_line(output, 0u, &line, &length)) {
        if (error && error_capacity > 0u) snprintf(error, error_capacity, "Git returned an empty repository root");
        free(output);
        return false;
    }
    git_copy_span(root, root_capacity, line, length);
    if (paths) {
        memset(paths, 0, sizeof(*paths));
        const char *git_line = NULL;
        const char *common_line = NULL;
        size_t git_length = 0u;
        size_t common_length = 0u;
        if (git_output_line(output, 1u, &git_line, &git_length) &&
            git_resolve_git_path(path, git_line, git_length,
                                 paths->git_dir, sizeof(paths->git_dir))) {
            if (!git_output_line(output, 2u, &common_line, &common_length) ||
                !git_resolve_git_path(path, common_line, common_length,
                                      paths->common_dir, sizeof(paths->common_dir))) {
                git_copy_span(paths->common_dir, sizeof(paths->common_dir),
                              paths->git_dir, strlen(paths->git_dir));
            }
        }
    }
    free(output);
    return true;
}

/* Return the remainder of path below dir; see git_plugin.h. */
const char *git_path_below(const char *dir, const char *path)
{
    const size_t length = strlen(dir);
    if (length == 0u || strncmp(dir, path, length) != 0) return NULL;
    if (path[length] == '\0') return path + length;
    if (path[length] != '/') return NULL;
    return path + length + 1u;
}

/* Classify a path relative to a Git metadata directory. */
static GitWatchKind git_classify_metadata(const char *relative)
{
    const char *base = strrchr(relative, '/');
    base = base ? base + 1 : relative;
    const size_t base_length = strlen(base);
    if (base_length == 0u) return GIT_WATCH_METADATA;
    if (base_length >= 5u && strcmp(base + base_length - 5u, ".lock") == 0) {
        return GIT_WATCH_NONE;
    }
    static const char *const k_noise[] = {
        "objects", "logs", "hooks", "lfs", "fsmonitor--daemon"
    };
    const char *component = relative;
    const char *previous = NULL;
    size_t previous_length = 0u;
    while (*component) {
        const size_t span = strcspn(component, "/");
        if (span == 4u && strncmp(component, "refs", 4u) == 0) return GIT_WATCH_REFS;
        for (size_t i = 0u; i < sizeof(k_noise) / sizeof(k_noise[0]); ++i) {
            if (strlen(k_noise[i]) == span && strncmp(component, k_noise[i], span) == 0) {
                return GIT_WATCH_NONE;
            }
        }
        if (component[span] == '\0') break;
        previous = component;
        previous_length = span;
        component += span + 1u;
    }
    static const char *const k_ref_files[] = {
        "HEAD", "packed-refs", "MERGE_HEAD", "CHERRY_PICK_HEAD",
        "REVERT_HEAD", "REBASE_HEAD"
    };
    for (size_t i = 0u; i < sizeof(k_ref_files) / sizeof(k_ref_files[0]); ++i) {
        if (strcmp(base, k_ref_files[i]) == 0) return GIT_WATCH_REFS;
    }
    static const char *const k_quiet_files[] = {
        "FETCH_HEAD", "ORIG_HEAD", "COMMIT_EDITMSG", "gc.log", "description",
        "shallow"
    };
    for (size_t i = 0u; i < sizeof(k_quiet_files) / sizeof(k_quiet_files[0]); ++i) {
        if (strcmp(base, k_quiet_files[i]) == 0) return GIT_WATCH_NONE;
    }
    if (strcmp(base, "exclude") == 0 && previous && previous_length == 4u &&
        strncmp(previous, "info", 4u) == 0) {
        return GIT_WATCH_IGNORE_RULES;
    }
    return GIT_WATCH_METADATA;
}

/* Classify one changed path against a repository; see git_plugin.h. */
GitWatchKind git_watch_classify(const char *repo_root,
                                const GitRepoPaths *paths,
                                const char *path,
                                const char **relative)
{
    if (relative) *relative = NULL;
    if (!repo_root || !repo_root[0] || !path || !path[0]) return GIT_WATCH_NONE;
    if (paths) {
        const char *below = paths->git_dir[0] ? git_path_below(paths->git_dir, path) : NULL;
        if (!below && paths->common_dir[0]) below = git_path_below(paths->common_dir, path);
        if (below) return git_classify_metadata(below);
    }
    const char *below = git_path_below(repo_root, path);
    if (!below) return GIT_WATCH_NONE;
    const char *component = below;
    while (*component) {
        const size_t span = strcspn(component, "/");
        if (span == 4u && strncmp(component, ".git", 4u) == 0) {
            return component[span] == '\0' ? GIT_WATCH_METADATA
                                           : git_classify_metadata(component + span + 1u);
        }
        if (component[span] == '\0') break;
        component += span + 1u;
    }
    const char *base = strrchr(below, '/');
    base = base ? base + 1 : below;
    if (strcmp(base, ".gitignore") == 0) return GIT_WATCH_IGNORE_RULES;
    if (relative) *relative = below;
    return GIT_WATCH_WORKTREE;
}

/* Report which candidate directories Git ignores; see git_plugin.h. */
void git_model_check_ignored(const char *root,
                             const char dirs[][GIT_WATCH_REL_CAP],
                             size_t count,
                             bool *ignored)
{
    if (!ignored) return;
    for (size_t i = 0u; i < count; ++i) ignored[i] = false;
    if (!root || !dirs || count == 0u || count > GIT_WATCH_DIR_CAP) return;
    const char *argv[GIT_WATCH_DIR_CAP + 4u];
    size_t argc = 0u;
    argv[argc++] = "git";
    argv[argc++] = "check-ignore";
    argv[argc++] = "--";
    for (size_t i = 0u; i < count; ++i) argv[argc++] = dirs[i];
    argv[argc] = NULL;
    const size_t capacity = GIT_WATCH_DIR_CAP * (GIT_WATCH_REL_CAP + 1u) + 1u;
    char *output = (char *)calloc(capacity, 1u);
    if (!output) return;
    GitProcessResult result = git_process_run(root, argv, output, capacity, 15000u);
    if (result.exit_code == 0 && !result.truncated) {
        const char *line = NULL;
        size_t length = 0u;
        for (size_t index = 0u; git_output_line(output, index, &line, &length); ++index) {
            for (size_t i = 0u; i < count; ++i) {
                if (strlen(dirs[i]) == length && strncmp(dirs[i], line, length) == 0) {
                    ignored[i] = true;
                }
            }
        }
    }
    free(output);
}

/* Refresh branch metadata and working-tree status. */
bool git_model_refresh(const char *root,
                       GitSnapshot *snapshot,
                       char *error,
                       size_t error_capacity)
{
    if (!root || !snapshot) return false;
    char *output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!output) return false;
    const char *argv[] = {
        "git", "--no-optional-locks", "status", "--porcelain=v2", "--branch", "-z",
        "--untracked-files=all", NULL
    };
    GitProcessResult result = git_process_run(root, argv, output,
                                              GIT_PROCESS_OUTPUT_CAP, 30000u);
    if (result.exit_code != 0 || result.truncated) {
        if (result.truncated && error && error_capacity > 0u) {
            snprintf(error, error_capacity, "Git status output exceeded %u bytes",
                     GIT_PROCESS_OUTPUT_CAP - 1u);
        } else {
            git_command_error(error, error_capacity, "Git status", &result, output);
        }
        free(output);
        return false;
    }
    char root_buf[GIT_PATH_CAP];
    git_copy_span(root_buf, sizeof(root_buf), root, strlen(root));
    memset(snapshot, 0, sizeof(*snapshot));
    git_copy_span(snapshot->root, sizeof(snapshot->root), root_buf, strlen(root_buf));
    const bool parsed = git_model_parse_status(output, result.output_len,
                                               snapshot, error, error_capacity) &&
                        git_model_refresh_submodules(root, snapshot, error,
                                                     error_capacity);
    free(output);
    return parsed;
}

/* Split a space-separated list of parent hashes (git log's %P) into up to
 * GIT_MAX_PARENTS entries on the commit entry. */
static void git_parse_parent_hashes(GitCommitEntry *entry,
                                    const char *field,
                                    const char *field_end)
{
    entry->parent_count = 0u;
    const char *p = field;
    while (p < field_end && entry->parent_count < GIT_MAX_PARENTS) {
        while (p < field_end && *p == ' ') ++p;
        const char *start = p;
        while (p < field_end && *p != ' ') ++p;
        if (p > start) {
            git_copy_span(entry->parents[entry->parent_count],
                         sizeof(entry->parents[0]), start,
                         (size_t)(p - start));
            entry->parent_count++;
        }
    }
}

bool git_model_parse_history(const char *data,
                             size_t length,
                             GitHistory *history,
                             char *error,
                             size_t error_capacity)
{
    if (!data || !history) return false;
    memset(history, 0, sizeof(*history));
    const char *cursor = data;
    const char *limit = data + length;
    while (cursor < limit && history->count < GIT_MAX_COMMITS) {
        while (cursor < limit && *cursor == '\0') ++cursor;
        if (cursor == limit) break;
        const char *fields[6] = {0};
        const char *ends[6] = {0};
        for (size_t field = 0u; field < 6u; ++field) {
            fields[field] = cursor;
            ends[field] = memchr(cursor, '\0', (size_t)(limit - cursor));
            if (!ends[field]) {
                if (error && error_capacity > 0u) {
                    snprintf(error, error_capacity, "Malformed NUL-delimited Git history");
                }
                return false;
            }
            cursor = ends[field] + 1u;
        }
        GitCommitEntry *entry = &history->commits[history->count++];
        git_copy_span(entry->hash, sizeof(entry->hash), fields[0],
                      (size_t)(ends[0] - fields[0]));
        git_copy_span(entry->short_hash, sizeof(entry->short_hash), fields[1],
                      (size_t)(ends[1] - fields[1]));
        git_copy_span(entry->author, sizeof(entry->author), fields[2],
                      (size_t)(ends[2] - fields[2]));
        git_copy_span(entry->date, sizeof(entry->date), fields[3],
                      (size_t)(ends[3] - fields[3]));
        git_parse_parent_hashes(entry, fields[4], ends[4]);
        git_copy_span(entry->subject, sizeof(entry->subject), fields[5],
                      (size_t)(ends[5] - fields[5]));
    }
    git_model_layout_graph(history);
    return true;
}

bool git_model_history(const char *root,
                       GitHistory *history,
                       char *error,
                       size_t error_capacity)
{
    if (!root || !history) return false;
    char *output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!output) return false;
    const char *argv[] = {
        "git", "log", "-z", "--max-count=128", "--date=short",
        "--pretty=format:%H%x00%h%x00%an%x00%ad%x00%P%x00%s%x00", NULL
    };
    GitProcessResult result = git_process_run(root, argv, output,
                                              GIT_PROCESS_OUTPUT_CAP, 30000u);
    if (result.exit_code != 0 || result.truncated) {
        if (result.truncated && error && error_capacity > 0u) {
            snprintf(error, error_capacity, "Git history output exceeded %u bytes",
                     GIT_PROCESS_OUTPUT_CAP - 1u);
        } else {
            git_command_error(error, error_capacity, "Git log", &result, output);
        }
        free(output);
        return false;
    }
    const bool parsed = git_model_parse_history(output, result.output_len,
                                                history, error, error_capacity);
    free(output);
    return parsed;
}

/* Return true when two commit hashes are the same non-empty hash. */
static bool git_hash_eq(const char *a, const char *b)
{
    return a[0] != '\0' && b[0] != '\0' && strcmp(a, b) == 0;
}

/* Assign vertical graph lanes to a commit list in reverse-chronological
 * (newest-first) order — the same order `git log` returns.
 *
 * One pass, tracking which commit hash each active lane is "waiting for"
 * (i.e. the next commit, further down the list, that will continue that
 * lane). For each commit: find the lane already waiting for it (or open a
 * new one for a fresh branch tip), record every currently-active lane as
 * "passing through" this row for line drawing, then update lane
 * assignments for its parents — first parent continues the same lane,
 * additional parents (merges) each claim an existing lane already waiting
 * for them or open a new one. Trailing free lanes are trimmed so closed
 * branches don't hold a column open forever.
 */
void git_model_layout_graph(GitHistory *history)
{
    if (!history) return;
    char lanes[GIT_MAX_GRAPH_LANES][GIT_HASH_CAP];
    size_t lane_count = 0u;
    for (size_t i = 0u; i < GIT_MAX_GRAPH_LANES; ++i) lanes[i][0] = '\0';

    for (size_t i = 0u; i < history->count; ++i) {
        GitCommitEntry *commit = &history->commits[i];

        int my_lane = -1;
        for (size_t lane = 0u; lane < lane_count; ++lane) {
            if (git_hash_eq(lanes[lane], commit->hash)) {
                my_lane = (int)lane;
                break;
            }
        }
        if (my_lane < 0 && lane_count < GIT_MAX_GRAPH_LANES) {
            my_lane = (int)lane_count++;
        }
        /* my_lane stays -1 when every lane is already claimed by a
           different, still-open branch. Forcing this commit onto the last
           lane regardless (the previous behavior) would clobber that
           lane's tracked hash, permanently losing track of the branch it
           belonged to and drawing its line as if it ended here — the
           renderer already treats a negative lane as "don't draw" (see
           the entry->lane < 0 checks in plugin.c), the same sentinel
           already used below for an overflowing merge parent. */
        commit->lane = my_lane;

        uint32_t through = 0u;
        for (size_t lane = 0u; lane < lane_count; ++lane) {
            if (lanes[lane][0]) through |= (1u << lane);
        }
        if (my_lane >= 0) through |= (1u << (uint32_t)my_lane);
        commit->through_lanes = through;

        if (my_lane >= 0) {
            lanes[my_lane][0] = '\0';
            if (commit->parent_count > 0u) {
                git_copy_span(lanes[my_lane], sizeof(lanes[my_lane]),
                             commit->parents[0], strlen(commit->parents[0]));
                commit->parent_lanes[0] = my_lane;
            }
        } else if (commit->parent_count > 0u) {
            commit->parent_lanes[0] = -1;
        }
        for (size_t p = 1u; p < commit->parent_count; ++p) {
            int parent_lane = -1;
            for (size_t lane = 0u; lane < lane_count; ++lane) {
                if (git_hash_eq(lanes[lane], commit->parents[p])) {
                    parent_lane = (int)lane;
                    break;
                }
            }
            if (parent_lane < 0 && lane_count < GIT_MAX_GRAPH_LANES) {
                parent_lane = (int)lane_count;
                git_copy_span(lanes[(size_t)parent_lane], sizeof(lanes[0]),
                             commit->parents[p], strlen(commit->parents[p]));
                ++lane_count;
            }
            commit->parent_lanes[p] = parent_lane;
        }
        while (lane_count > 0u && lanes[lane_count - 1u][0] == '\0') {
            --lane_count;
        }
    }
}

/* Load local branches and upstream names. */
bool git_model_branches(const char *root,
                        GitBranches *branches,
                        char *error,
                        size_t error_capacity)
{
    if (!root || !branches) return false;
    memset(branches, 0, sizeof(*branches));
    char *output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!output) return false;
    const char *argv[] = {
        "git", "for-each-ref", "--sort=refname",
        "--format=%(refname:short)%09%(upstream:short)%09%(HEAD)",
        "refs/heads", NULL
    };
    GitProcessResult result = git_process_run(root, argv, output,
                                              GIT_PROCESS_OUTPUT_CAP, 30000u);
    if (result.exit_code != 0) {
        git_command_error(error, error_capacity, "Git branch list", &result, output);
        free(output);
        return false;
    }

    char *line = output;
    while (*line && branches->count < GIT_MAX_BRANCHES) {
        char *end = strpbrk(line, "\r\n");
        if (end) *end = '\0';
        char *first_tab = strchr(line, '\t');
        char *second_tab = first_tab ? strchr(first_tab + 1, '\t') : NULL;
        if (first_tab && second_tab) {
            *first_tab = '\0';
            *second_tab = '\0';
            GitBranchEntry *entry = &branches->branches[branches->count++];
            git_copy_span(entry->name, sizeof(entry->name), line, strlen(line));
            git_copy_span(entry->upstream, sizeof(entry->upstream),
                          first_tab + 1, strlen(first_tab + 1));
            entry->current = second_tab[1] == '*';
        }
        if (!end) break;
        line = end + 1;
        while (*line == '\r' || *line == '\n') ++line;
    }
    free(output);
    return true;
}
