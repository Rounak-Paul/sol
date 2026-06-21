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
        git_append_file(snapshot, kind, index_status, worktree_status,
                        path, path_length, original, original_length);
        return true;
    }
    return false;
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

/* Discover the containing repository for a workspace path. */
bool git_model_discover(const char *path,
                        char *root,
                        size_t root_capacity,
                        char *error,
                        size_t error_capacity)
{
    if (!path || !path[0] || !root || root_capacity == 0u) return false;
    char *output = (char *)calloc(GIT_PATH_CAP, 1u);
    if (!output) return false;
    const char *argv[] = { "git", "rev-parse", "--show-toplevel", NULL };
    GitProcessResult result = git_process_run(path, argv, output, GIT_PATH_CAP, 15000u);
    if (result.exit_code != 0) {
        git_command_error(error, error_capacity, "Repository discovery", &result, output);
        free(output);
        return false;
    }
    size_t length = strcspn(output, "\r\n");
    if (length == 0u) {
        if (error && error_capacity > 0u) snprintf(error, error_capacity, "Git returned an empty repository root");
        free(output);
        return false;
    }
    git_copy_span(root, root_capacity, output, length);
    free(output);
    return true;
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
        "git", "status", "--porcelain=v2", "--branch", "-z",
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
                                               snapshot, error, error_capacity);
    free(output);
    return parsed;
}

/* Load recent commit history using control-character field delimiters. */
bool git_model_history(const char *root,
                       GitHistory *history,
                       char *error,
                       size_t error_capacity)
{
    if (!root || !history) return false;
    memset(history, 0, sizeof(*history));
    char *output = (char *)calloc(GIT_PROCESS_OUTPUT_CAP, 1u);
    if (!output) return false;
    const char *argv[] = {
        "git", "log", "--max-count=128", "--date=short",
        "--pretty=format:%H%x1f%h%x1f%an%x1f%ad%x1f%s%x1e", NULL
    };
    GitProcessResult result = git_process_run(root, argv, output,
                                              GIT_PROCESS_OUTPUT_CAP, 30000u);
    if (result.exit_code != 0) {
        git_command_error(error, error_capacity, "Git log", &result, output);
        free(output);
        return false;
    }

    const char *cursor = output;
    const char *limit = output + result.output_len;
    while (cursor < limit && history->count < GIT_MAX_COMMITS) {
        const char *record_end = memchr(cursor, 0x1e, (size_t)(limit - cursor));
        if (!record_end) record_end = limit;
        const char *fields[5] = { cursor, NULL, NULL, NULL, NULL };
        const char *scan = cursor;
        for (size_t i = 1u; i < 5u; ++i) {
            scan = memchr(scan, 0x1f, (size_t)(record_end - scan));
            if (!scan) break;
            fields[i] = ++scan;
        }
        if (fields[4]) {
            GitCommitEntry *entry = &history->commits[history->count++];
            const char *ends[5] = { fields[1] - 1, fields[2] - 1,
                                    fields[3] - 1, fields[4] - 1, record_end };
            git_copy_span(entry->hash, sizeof(entry->hash), fields[0],
                          (size_t)(ends[0] - fields[0]));
            git_copy_span(entry->short_hash, sizeof(entry->short_hash), fields[1],
                          (size_t)(ends[1] - fields[1]));
            git_copy_span(entry->author, sizeof(entry->author), fields[2],
                          (size_t)(ends[2] - fields[2]));
            git_copy_span(entry->date, sizeof(entry->date), fields[3],
                          (size_t)(ends[3] - fields[3]));
            git_copy_span(entry->subject, sizeof(entry->subject), fields[4],
                          (size_t)(ends[4] - fields[4]));
        }
        cursor = record_end < limit ? record_end + 1u : limit;
    }
    free(output);
    return true;
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
