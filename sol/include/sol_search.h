// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#ifndef SOL_SEARCH_H
#define SOL_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

typedef struct SolSearchIndex SolSearchIndex;
typedef struct SolSearchResult SolSearchResult;
typedef bool (*SolSearchProgressFn)(const SolSearchResult *results,
                                    size_t result_count,
                                    size_t processed_files,
                                    size_t total_files,
                                    void *user_data);

struct SolSearchResult {
    const char *full_path;
    const char *relative_path;
    size_t      line_number;   /* 1-based; 0 for file-search results */
    int         score;
    char        preview[256];
};

SolSearchIndex *sol_search_index_create(const char *root_path);
void            sol_search_index_destroy(SolSearchIndex *index);

const char *sol_search_index_root(const SolSearchIndex *index);
size_t      sol_search_index_file_count(const SolSearchIndex *index);

/* Returns a negative value when query is not a fuzzy subsequence match. */
int sol_search_fuzzy_score(const char *candidate, const char *query);

size_t sol_search_files(const SolSearchIndex *index,
                        const char *query,
                        SolSearchResult *results,
                        size_t result_capacity);

size_t sol_search_contents(const SolSearchIndex *index,
                           const char *query,
                           SolSearchResult *results,
                           size_t result_capacity);

size_t sol_search_contents_progress(const SolSearchIndex *index,
                                    const char *query,
                                    SolSearchResult *results,
                                    size_t result_capacity,
                                    SolSearchProgressFn progress,
                                    void *progress_data);

#endif /* SOL_SEARCH_H */
