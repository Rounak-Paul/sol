// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#ifndef SOL_SEARCH_H
#define SOL_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

typedef struct SolSearchIndex SolSearchIndex;
typedef struct SolSearchResult SolSearchResult;

/*
 * Incremental progress callback invoked by sol_search_contents_progress.
 *
 * results          Partial results array gathered so far.
 * result_count     Number of valid entries in results.
 * processed_files  Files examined so far.
 * total_files      Total files in the index.
 * user_data        Caller-supplied context.
 * Returns          false to abort the search early.
 */
typedef bool (*SolSearchProgressFn)(const SolSearchResult *results,
                                    size_t result_count,
                                    size_t processed_files,
                                    size_t total_files,
                                    void *user_data);

/* A single search hit returned by the file or content search functions. */
struct SolSearchResult {
    const char *full_path;
    const char *relative_path;
    size_t      line_number;   /* 1-based; 0 for file-search results */
    int         score;
    char        preview[256];
};

/*
 * Create a search index rooted at a directory.
 *
 * root_path  Absolute path of the directory to index.
 * Returns    A heap-allocated index, or NULL on failure.
 */
SolSearchIndex *sol_search_index_create(const char *root_path);

/* Destroy a search index and free all resources. */
void            sol_search_index_destroy(SolSearchIndex *index);

/* Returns the root directory path this index was created with. */
const char *sol_search_index_root(const SolSearchIndex *index);

/* Returns the number of files in the index. */
size_t      sol_search_index_file_count(const SolSearchIndex *index);

/*
 * Compute a fuzzy match score for a candidate string against a query.
 *
 * candidate  String to score.
 * query      Query subsequence to match.
 * Returns    A non-negative score on match; negative when not a subsequence.
 */
int sol_search_fuzzy_score(const char *candidate, const char *query);

/*
 * Search file paths in the index by fuzzy subsequence match.
 *
 * index            The search index.
 * query            Fuzzy query string.
 * results          Output array to fill.
 * result_capacity  Maximum number of results to write.
 * Returns          Number of results written.
 */
size_t sol_search_files(const SolSearchIndex *index,
                        const char *query,
                        SolSearchResult *results,
                        size_t result_capacity);

/*
 * Search file contents in the index for lines matching query.
 *
 * index            The search index.
 * query            Search string.
 * results          Output array to fill.
 * result_capacity  Maximum number of results to write.
 * Returns          Number of results written.
 */
size_t sol_search_contents(const SolSearchIndex *index,
                           const char *query,
                           SolSearchResult *results,
                           size_t result_capacity);

/*
 * Search file contents with incremental progress callbacks.
 *
 * index            The search index.
 * query            Search string.
 * results          Output array to fill.
 * result_capacity  Maximum number of results to write.
 * progress         Optional callback invoked after each file; may be NULL.
 * progress_data    Passed unchanged to progress.
 * Returns          Number of results written.
 */
size_t sol_search_contents_progress(const SolSearchIndex *index,
                                    const char *query,
                                    SolSearchResult *results,
                                    size_t result_capacity,
                                    SolSearchProgressFn progress,
                                    void *progress_data);

#endif /* SOL_SEARCH_H */
