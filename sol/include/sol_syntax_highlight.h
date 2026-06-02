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

/* A single colored byte range within the document.
 * `css_class` points to a string literal — never freed. */
typedef struct SolSyntaxSpan {
    uint32_t    start_byte;
    uint32_t    end_byte;
    const char *css_class;
} SolSyntaxSpan;

typedef struct SolSyntaxHighlighter SolSyntaxHighlighter;

/* ---- Lifecycle ---------------------------------------------------- */

/* Create a highlighter for the given TSLanguage* (passed as void* to
 * keep this header tree-sitter-free).  `query_text` is the raw text of
 * a highlights.scm query to compile; pass NULL to fall back to the
 * built-in node-type heuristic walker.
 * Returns NULL on allocation failure or if language is NULL. */
SolSyntaxHighlighter *sol_syntax_highlight_create(const void *language,
                                                   const char *query_text);
void                  sol_syntax_highlight_destroy(SolSyntaxHighlighter *h);

/* ---- Parsing ------------------------------------------------------ */

/* (Re)parse the rope content and rebuild the internal span table.
 * Call this after the rope has been mutated.
 * No-op when h or rope is NULL. */
void sol_syntax_highlight_reparse(SolSyntaxHighlighter *h,
                                   const SolRope        *rope);

/* ---- Query -------------------------------------------------------- */

/* Fill `out` with spans that overlap the byte range
 * [line_start_byte, line_end_byte).  Returns the number of spans written,
 * capped at max_out.  Spans are ordered by start_byte. */
size_t sol_syntax_highlight_spans_for_range(
    const SolSyntaxHighlighter *h,
    uint32_t                    line_start_byte,
    uint32_t                    line_end_byte,
    SolSyntaxSpan              *out,
    size_t                      max_out);

/* True if the last reparse succeeded and the span table is valid. */
bool sol_syntax_highlight_is_valid(const SolSyntaxHighlighter *h);

/* Return the TSLanguage* (as void*) this highlighter was created with.
 * Used by the plugin system to find buffers that must be invalidated
 * when the language's owning plugin is unloaded.                     */
const void *sol_syntax_highlight_get_language(const SolSyntaxHighlighter *h);

#ifdef __cplusplus
}
#endif

#endif /* SOL_SYNTAX_HIGHLIGHT_H */
