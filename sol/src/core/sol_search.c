// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_search.h"

#include "sol_platform.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SOL_SEARCH_MAX_FILES       50000u
#define SOL_SEARCH_MAX_DEPTH       64u
#define SOL_SEARCH_MAX_FILE_BYTES  (2u * 1024u * 1024u)

typedef struct SolSearchFile {
    char *full_path;
    char *relative_path;
    size_t size_bytes;
} SolSearchFile;

struct SolSearchIndex {
    char          *root_path;
    SolSearchFile *files;
    size_t         file_count;
    size_t         file_capacity;
};

static char *search_strdup(const char *text)
{
    if (!text) return NULL;
    const size_t n = strlen(text);
    char *copy = (char *)malloc(n + 1u);
    if (copy) memcpy(copy, text, n + 1u);
    return copy;
}

static bool search_is_ignored_dir(const char *name)
{
    if (!name || !name[0]) return true;
    if (name[0] == '.') return true;
    return strcmp(name, "build") == 0 ||
           strcmp(name, "bin") == 0 ||
           strcmp(name, "node_modules") == 0;
}

static bool search_index_add_file(SolSearchIndex *index,
                                  const char *full_path,
                                  const char *relative_path,
                                  size_t size_bytes)
{
    if (index->file_count >= SOL_SEARCH_MAX_FILES) return false;
    if (index->file_count == index->file_capacity) {
        size_t cap = index->file_capacity ? index->file_capacity * 2u : 256u;
        if (cap > SOL_SEARCH_MAX_FILES) cap = SOL_SEARCH_MAX_FILES;
        SolSearchFile *grown = (SolSearchFile *)realloc(
            index->files, cap * sizeof(SolSearchFile));
        if (!grown) return false;
        index->files = grown;
        index->file_capacity = cap;
    }

    SolSearchFile *file = &index->files[index->file_count];
    file->full_path = search_strdup(full_path);
    file->relative_path = search_strdup(relative_path);
    if (!file->full_path || !file->relative_path) {
        free(file->full_path);
        free(file->relative_path);
        return false;
    }
    file->size_bytes = size_bytes;
    index->file_count++;
    return true;
}

static void search_index_walk(SolSearchIndex *index,
                              const char *directory,
                              const char *relative_directory,
                              size_t depth)
{
    if (!index || depth > SOL_SEARCH_MAX_DEPTH ||
        index->file_count >= SOL_SEARCH_MAX_FILES) {
        return;
    }

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, directory)) return;

    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        if (index->file_count >= SOL_SEARCH_MAX_FILES) break;
        if (entry.name[0] == '.') continue;

        char *full = sol_platform_path_join(directory, entry.name);
        char *relative = relative_directory && relative_directory[0]
            ? sol_platform_path_join(relative_directory, entry.name)
            : search_strdup(entry.name);
        if (!full || !relative) {
            free(full);
            free(relative);
            continue;
        }

        if (entry.is_directory) {
            if (!search_is_ignored_dir(entry.name)) {
                search_index_walk(index, full, relative, depth + 1u);
            }
        } else {
            SolPathInfo info;
            if (sol_platform_get_path_info(full, &info) && info.is_regular_file) {
                (void)search_index_add_file(
                    index, full, relative, info.size_bytes);
            }
        }
        free(full);
        free(relative);
    }
    sol_platform_dir_close(&iter);
}

SolSearchIndex *sol_search_index_create(const char *root_path)
{
    if (!root_path || !root_path[0]) return NULL;
    SolPathInfo info;
    if (!sol_platform_get_path_info(root_path, &info) || !info.is_directory) {
        return NULL;
    }

    SolSearchIndex *index = (SolSearchIndex *)calloc(1u, sizeof(SolSearchIndex));
    if (!index) return NULL;
    index->root_path = search_strdup(root_path);
    if (!index->root_path) {
        free(index);
        return NULL;
    }
    search_index_walk(index, root_path, "", 0u);
    return index;
}

void sol_search_index_destroy(SolSearchIndex *index)
{
    if (!index) return;
    for (size_t i = 0u; i < index->file_count; ++i) {
        free(index->files[i].full_path);
        free(index->files[i].relative_path);
    }
    free(index->files);
    free(index->root_path);
    free(index);
}

const char *sol_search_index_root(const SolSearchIndex *index)
{
    return index ? index->root_path : NULL;
}

size_t sol_search_index_file_count(const SolSearchIndex *index)
{
    return index ? index->file_count : 0u;
}

int sol_search_fuzzy_score(const char *candidate, const char *query)
{
    if (!candidate || !query) return -1;
    if (!query[0]) return 0;

    int score = 0;
    int previous = -2;
    size_t qi = 0u;
    for (size_t ci = 0u; candidate[ci] && query[qi]; ++ci) {
        const int cc = tolower((unsigned char)candidate[ci]);
        const int qc = tolower((unsigned char)query[qi]);
        if (cc != qc) continue;

        score += 10;
        if ((int)ci == previous + 1) score += 12;
        if (ci == 0u || candidate[ci - 1u] == '/' ||
            candidate[ci - 1u] == '\\' || candidate[ci - 1u] == '_' ||
            candidate[ci - 1u] == '-' || candidate[ci - 1u] == '.') {
            score += 18;
        }
        if (candidate[ci] == query[qi]) score += 2;
        previous = (int)ci;
        qi++;
    }
    if (query[qi]) return -1;
    score -= (int)strlen(candidate) / 4;
    return score;
}

static int search_result_compare(const void *a, const void *b)
{
    const SolSearchResult *x = (const SolSearchResult *)a;
    const SolSearchResult *y = (const SolSearchResult *)b;
    if (x->score != y->score) return y->score - x->score;
    if (x->line_number != y->line_number) {
        return x->line_number < y->line_number ? -1 : 1;
    }
    return strcmp(x->relative_path, y->relative_path);
}

static void search_add_ranked(SolSearchResult *results,
                              size_t *count,
                              size_t capacity,
                              const SolSearchResult *candidate)
{
    if (capacity == 0u) return;
    if (*count < capacity) {
        results[(*count)++] = *candidate;
        return;
    }
    size_t worst = 0u;
    for (size_t i = 1u; i < *count; ++i) {
        if (results[i].score < results[worst].score) worst = i;
    }
    if (candidate->score > results[worst].score) results[worst] = *candidate;
}

size_t sol_search_files(const SolSearchIndex *index,
                        const char *query,
                        SolSearchResult *results,
                        size_t result_capacity)
{
    if (!index || !results || result_capacity == 0u) return 0u;
    if (!query) query = "";

    size_t count = 0u;
    for (size_t i = 0u; i < index->file_count; ++i) {
        const int score = sol_search_fuzzy_score(index->files[i].relative_path, query);
        if (score < 0) continue;
        SolSearchResult result = {0};
        result.full_path = index->files[i].full_path;
        result.relative_path = index->files[i].relative_path;
        result.score = score;
        search_add_ranked(results, &count, result_capacity, &result);
    }
    qsort(results, count, sizeof(SolSearchResult), search_result_compare);
    return count;
}

static const uint8_t *search_find_nocase(const uint8_t *text,
                                         size_t text_len,
                                         const uint8_t *query_lower,
                                         size_t query_len,
                                         const size_t skip[256])
{
    if (query_len == 0u || text_len < query_len) return NULL;
    size_t offset = 0u;
    while (offset + query_len <= text_len) {
        size_t j = query_len;
        while (j > 0u) {
            const size_t at = j - 1u;
            if ((uint8_t)tolower(text[offset + at]) != query_lower[at]) break;
            j--;
        }
        if (j == 0u) return text + offset;
        offset += skip[(uint8_t)tolower(text[offset + query_len - 1u])];
    }
    return NULL;
}

static bool search_looks_binary(const uint8_t *data, size_t size)
{
    const size_t inspect = size < 4096u ? size : 4096u;
    for (size_t i = 0u; i < inspect; ++i) {
        if (data[i] == 0u) return true;
    }
    return false;
}

size_t sol_search_contents_progress(const SolSearchIndex *index,
                                    const char *query,
                                    SolSearchResult *results,
                                    size_t result_capacity,
                                    SolSearchProgressFn progress,
                                    void *progress_data)
{
    if (!index || !query || !query[0] || !results || result_capacity == 0u) {
        return 0u;
    }

    const size_t query_len = strlen(query);
    uint8_t *query_lower = (uint8_t *)malloc(query_len);
    if (!query_lower) return 0u;
    size_t skip[256];
    for (size_t i = 0u; i < 256u; ++i) skip[i] = query_len;
    for (size_t i = 0u; i < query_len; ++i) {
        query_lower[i] = (uint8_t)tolower((unsigned char)query[i]);
        if (i + 1u < query_len) skip[query_lower[i]] = query_len - i - 1u;
    }

    size_t count = 0u;
    bool completed = true;
    for (size_t fi = 0u; fi < index->file_count; ++fi) {
        if (progress &&
            !progress(results, count, fi, index->file_count, progress_data)) {
            completed = false;
            break;
        }
        if (index->files[fi].size_bytes > SOL_SEARCH_MAX_FILE_BYTES) {
            continue;
        }

        SolMappedFile mapped;
        if (!sol_platform_map_file_readonly(index->files[fi].full_path, &mapped, NULL)) {
            continue;
        }
        if (search_looks_binary(mapped.data, mapped.size_bytes)) {
            sol_platform_unmap_file(&mapped);
            continue;
        }

        size_t line_number = 1u;
        size_t line_start = 0u;
        size_t next_cancel_check = 64u * 1024u;
        while (line_start < mapped.size_bytes) {
            size_t line_end = line_start;
            while (line_end < mapped.size_bytes && mapped.data[line_end] != '\n') {
                line_end++;
                if (progress && line_end >= next_cancel_check) {
                    if (!progress(
                            results, count, fi, index->file_count, progress_data)) {
                        completed = false;
                        break;
                    }
                    next_cancel_check = line_end + 64u * 1024u;
                }
            }
            if (!completed) break;
            const uint8_t *match = search_find_nocase(
                mapped.data + line_start, line_end - line_start,
                query_lower, query_len, skip);
            if (match) {
                SolSearchResult result = {0};
                result.full_path = index->files[fi].full_path;
                result.relative_path = index->files[fi].relative_path;
                result.line_number = line_number;
                const size_t match_column = (size_t)(match - (mapped.data + line_start));
                int path_score =
                    sol_search_fuzzy_score(index->files[fi].relative_path, query);
                if (path_score < 0) path_score = 0;
                result.score = 1000 - (int)match_column + path_score / 4;

                size_t preview_start = line_start;
                while (preview_start < line_end &&
                       (mapped.data[preview_start] == ' ' ||
                        mapped.data[preview_start] == '\t')) {
                    preview_start++;
                }
                size_t preview_len = line_end - preview_start;
                if (preview_len > 0u &&
                    mapped.data[preview_start + preview_len - 1u] == '\r') {
                    preview_len--;
                }
                if (preview_len >= sizeof(result.preview)) {
                    preview_len = sizeof(result.preview) - 1u;
                }
                memcpy(result.preview, mapped.data + preview_start, preview_len);
                result.preview[preview_len] = '\0';
                search_add_ranked(results, &count, result_capacity, &result);
            }
            line_start = line_end + 1u;
            line_number++;
        }
        sol_platform_unmap_file(&mapped);
        if (!completed) break;
        if (progress &&
            !progress(results, count, fi + 1u, index->file_count, progress_data)) {
            completed = false;
            break;
        }
    }
    qsort(results, count, sizeof(SolSearchResult), search_result_compare);
    if (progress && completed) {
        (void)progress(
            results, count, index->file_count, index->file_count, progress_data);
    }
    free(query_lower);
    return count;
}

size_t sol_search_contents(const SolSearchIndex *index,
                           const char *query,
                           SolSearchResult *results,
                           size_t result_capacity)
{
    return sol_search_contents_progress(
        index, query, results, result_capacity, NULL, NULL);
}
