// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#ifndef SOL_MARKDOWN_H
#define SOL_MARKDOWN_H

#include <stdbool.h>
#include <stddef.h>

typedef enum SolMarkdownInlineStyle {
    SOL_MARKDOWN_INLINE_PLAIN = 0,
    SOL_MARKDOWN_INLINE_EMPHASIS,
    SOL_MARKDOWN_INLINE_STRONG,
    SOL_MARKDOWN_INLINE_STRIKETHROUGH,
    SOL_MARKDOWN_INLINE_CODE,
    SOL_MARKDOWN_INLINE_LINK,
} SolMarkdownInlineStyle;

typedef struct SolMarkdownInlineToken {
    size_t start_byte;
    size_t byte_length;
    SolMarkdownInlineStyle style;
} SolMarkdownInlineToken;

typedef enum SolMarkdownBlockKind {
    SOL_MARKDOWN_BLOCK_PARAGRAPH = 0,
    SOL_MARKDOWN_BLOCK_HEADING,
    SOL_MARKDOWN_BLOCK_QUOTE,
    SOL_MARKDOWN_BLOCK_UNORDERED_LIST,
    SOL_MARKDOWN_BLOCK_ORDERED_LIST,
    SOL_MARKDOWN_BLOCK_TASK,
    SOL_MARKDOWN_BLOCK_FENCE,
    SOL_MARKDOWN_BLOCK_CODE,
    SOL_MARKDOWN_BLOCK_RULE,
    SOL_MARKDOWN_BLOCK_TABLE,
    SOL_MARKDOWN_BLOCK_TABLE_SEPARATOR,
} SolMarkdownBlockKind;

typedef struct SolMarkdownParserState {
    bool in_fenced_code;
    char fence_character;
    size_t fence_length;
    char fence_language[32];
} SolMarkdownParserState;

typedef struct SolMarkdownBlock {
    SolMarkdownBlockKind kind;
    size_t content_start_byte;
    size_t content_byte_length;
    unsigned heading_level;
    bool task_checked;
    const char *fence_language;
} SolMarkdownBlock;

/* Initialize a line-stream parser before parsing a Markdown document. */
void sol_markdown_parser_init(SolMarkdownParserState *state);

/*
 * Parse one source line and advance state. The returned block references the
 * supplied line; it is valid only while that line remains valid.
 *
 * line  Null-terminated source line.
 * state In/out parser state for the surrounding document stream.
 */
SolMarkdownBlock sol_markdown_parse_line(const char *line,
                                         SolMarkdownParserState *state);

/*
 * Tokenize inline Markdown into visible source ranges. Delimiters are omitted
 * from styled tokens, so consumers can render the original document without
 * allocating a transformed copy. Returns the number of tokens written.
 *
 * text     Null-terminated source line.
 * out      Destination token array; may be NULL when out_capacity is zero.
 * out_capacity Number of token slots available in out.
 */
size_t sol_markdown_inline_tokens(const char *text,
                                  SolMarkdownInlineToken *out,
                                  size_t out_capacity);

#endif /* SOL_MARKDOWN_H */
