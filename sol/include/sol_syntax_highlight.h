// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_syntax_highlight.h — Tree-sitter based per-buffer syntax highlighter.
 *
 * A SolSyntaxHighlighter wraps a TSParser + TSTree and maintains a flat
 * sorted array of SolSyntaxSpan values covering every semantically colored
 * leaf node in the document.  The text view queries it per visible line to
 * produce a sequence of colored ca_text segments.
 *
 * The highlighter re-parses the full document on every edit.  Incremental
 * re-parsing (ts_tree_edit) can be added later without changing the API.
 */

#ifndef SOL_SYNTAX_HIGHLIGHT_H
#define SOL_SYNTAX_HIGHLIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — callers do not need tree_sitter/api.h. */
typedef struct SolRope SolRope;

/*
 * A single colored byte range within the document.
 *
 * css_class points to a string literal owned by the registry — never freed.
 */
typedef struct SolSyntaxSpan {
    uint32_t    start_byte;
    uint32_t    end_byte;
    const char *css_class;
} SolSyntaxSpan;

typedef struct SolSyntaxHighlighter SolSyntaxHighlighter;

/* ---- Lifecycle ---------------------------------------------------- */

/*
 * Create a syntax highlighter for a tree-sitter language.
 *
 * language    const TSLanguage* cast to const void*.
 * query_text  Raw text of a highlights.scm query to compile, or NULL to use
 *             the built-in node-type heuristic walker.
 * Returns     A heap-allocated highlighter, or NULL on failure or if language is NULL.
 */
SolSyntaxHighlighter *sol_syntax_highlight_create(const void *language,
                                                   const char *query_text);

/* Destroy the highlighter and free its internal tree and span table. */
void                  sol_syntax_highlight_destroy(SolSyntaxHighlighter *h);

/* ---- Parsing ------------------------------------------------------ */

/*
 * Reparse the rope content and rebuild the internal span table.
 *
 * Call this after the rope has been mutated. No-op when h or rope is NULL.
 *
 * h     The highlighter.
 * rope  The current rope content to parse.
 */
void sol_syntax_highlight_reparse(SolSyntaxHighlighter *h,
                                   const SolRope        *rope);

/* ---- Query -------------------------------------------------------- */

/*
 * Fill out with syntax spans overlapping a byte range.
 *
 * h                The highlighter.
 * line_start_byte  Inclusive start of the byte range to query.
 * line_end_byte    Exclusive end of the byte range to query.
 * out              Output array to fill with overlapping spans.
 * max_out          Maximum number of spans to write.
 * Returns          Number of spans written, capped at max_out.
 *                  Spans are ordered by start_byte.
 */
size_t sol_syntax_highlight_spans_for_range(
    const SolSyntaxHighlighter *h,
    uint32_t                    line_start_byte,
    uint32_t                    line_end_byte,
    SolSyntaxSpan              *out,
    size_t                      max_out);

/* Returns true if the last reparse succeeded and the span table is valid. */
bool sol_syntax_highlight_is_valid(const SolSyntaxHighlighter *h);

/*
 * Return the TSLanguage* (as void*) this highlighter was created with.
 *
 * Used by the plugin system to invalidate buffers when the language's
 * owning plugin is unloaded.
 */
const void *sol_syntax_highlight_get_language(const SolSyntaxHighlighter *h);

#ifdef __cplusplus
}
#endif

#endif /* SOL_SYNTAX_HIGHLIGHT_H */
