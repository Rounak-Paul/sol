// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_markdown.h"

#include <stdbool.h>
#include <string.h>

/* Copy a fence language identifier without accepting unbounded source text. */
static void markdown_set_fence_language(SolMarkdownParserState *state,
                                        const char *language)
{
    if (!state) return;
    size_t length = 0u;
    while (language && language[length] && language[length] != ' ' &&
           language[length] != '\t' && length + 1u < sizeof(state->fence_language)) {
        state->fence_language[length] = language[length];
        ++length;
    }
    state->fence_language[length] = '\0';
}

/* Return a horizontal-rule marker, or zero when line is not a rule. */
static char markdown_rule_marker(const char *line)
{
    char marker = '\0';
    size_t count = 0u;
    for (; line && *line; ++line) {
        if (*line == ' ' || *line == '\t') continue;
        if (*line != '-' && *line != '*' && *line != '_') return '\0';
        if (marker == '\0') marker = *line;
        if (*line != marker) return '\0';
        ++count;
    }
    return count >= 3u ? marker : '\0';
}

void sol_markdown_parser_init(SolMarkdownParserState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

SolMarkdownBlock sol_markdown_parse_line(const char *line,
                                         SolMarkdownParserState *state)
{
    SolMarkdownBlock block = { .kind = SOL_MARKDOWN_BLOCK_PARAGRAPH };
    if (!line || !state) return block;
    const size_t length = strlen(line);

    if (state->in_fenced_code) {
        size_t fence = 0u;
        while (line[fence] == state->fence_character) ++fence;
        if (fence >= state->fence_length) {
            state->in_fenced_code = false;
            block.kind = SOL_MARKDOWN_BLOCK_FENCE;
            block.fence_language = state->fence_language;
            return block;
        }
        block.kind = SOL_MARKDOWN_BLOCK_CODE;
        block.content_byte_length = length;
        block.fence_language = state->fence_language;
        return block;
    }

    if ((line[0] == '`' || line[0] == '~') && line[1] == line[0] && line[2] == line[0]) {
        size_t fence = 0u;
        while (line[fence] == line[0]) ++fence;
        state->in_fenced_code = true;
        state->fence_character = line[0];
        state->fence_length = fence;
        markdown_set_fence_language(state, line + fence);
        block.kind = SOL_MARKDOWN_BLOCK_FENCE;
        block.fence_language = state->fence_language;
        return block;
    }

    size_t heading = 0u;
    while (heading < 6u && line[heading] == '#') ++heading;
    if (heading > 0u && line[heading] == ' ') {
        block.kind = SOL_MARKDOWN_BLOCK_HEADING;
        block.heading_level = (unsigned)heading;
        block.content_start_byte = heading + 1u;
        block.content_byte_length = length - block.content_start_byte;
        return block;
    }
    if (line[0] == '>') {
        block.kind = SOL_MARKDOWN_BLOCK_QUOTE;
        block.content_start_byte = line[1] == ' ' ? 2u : 1u;
        block.content_byte_length = length - block.content_start_byte;
        return block;
    }
    if (markdown_rule_marker(line) != '\0') {
        block.kind = SOL_MARKDOWN_BLOCK_RULE;
        return block;
    }
    if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
        block.content_start_byte = 2u;
        if (line[2] == '[' && line[4] == ']' && line[5] == ' ') {
            block.kind = SOL_MARKDOWN_BLOCK_TASK;
            block.task_checked = line[3] == 'x' || line[3] == 'X';
            block.content_start_byte = 6u;
        } else {
            block.kind = SOL_MARKDOWN_BLOCK_UNORDERED_LIST;
        }
        block.content_byte_length = length - block.content_start_byte;
        return block;
    }
    size_t ordered = 0u;
    while (line[ordered] >= '0' && line[ordered] <= '9') ++ordered;
    if (ordered > 0u && line[ordered] == '.' && line[ordered + 1u] == ' ') {
        block.kind = SOL_MARKDOWN_BLOCK_ORDERED_LIST;
        block.content_start_byte = ordered + 2u;
        block.content_byte_length = length - block.content_start_byte;
        return block;
    }
    if (line[0] == '|' && strchr(line + 1, '|')) {
        block.kind = SOL_MARKDOWN_BLOCK_TABLE;
    }
    block.content_byte_length = length;
    return block;
}

/* Append a non-empty visible range when the caller has capacity. */
static void markdown_push(SolMarkdownInlineToken *out, size_t out_capacity,
                          size_t *count, size_t start, size_t length,
                          SolMarkdownInlineStyle style)
{
    if (!count || length == 0u || *count >= out_capacity) return;
    out[*count] = (SolMarkdownInlineToken){
        .start_byte = start, .byte_length = length, .style = style,
    };
    ++*count;
}

/* Return the style and delimiter length at text[position], when supported. */
static size_t markdown_delimiter(const char *text, size_t length, size_t position,
                                 SolMarkdownInlineStyle *out_style)
{
    if (!text || !out_style || position >= length) return 0u;
    if (position + 1u < length && text[position] == '*' && text[position + 1u] == '*') {
        *out_style = SOL_MARKDOWN_INLINE_STRONG; return 2u;
    }
    if (position + 1u < length && text[position] == '_' && text[position + 1u] == '_') {
        *out_style = SOL_MARKDOWN_INLINE_STRONG; return 2u;
    }
    if (position + 1u < length && text[position] == '~' && text[position + 1u] == '~') {
        *out_style = SOL_MARKDOWN_INLINE_STRIKETHROUGH; return 2u;
    }
    if (text[position] == '`') {
        *out_style = SOL_MARKDOWN_INLINE_CODE; return 1u;
    }
    if (text[position] == '*' || text[position] == '_') {
        *out_style = SOL_MARKDOWN_INLINE_EMPHASIS; return 1u;
    }
    return 0u;
}

size_t sol_markdown_inline_tokens(const char *text,
                                  SolMarkdownInlineToken *out,
                                  size_t out_capacity)
{
    if (!text || !out || out_capacity == 0u) return 0u;
    const size_t length = strlen(text);
    size_t count = 0u, plain_start = 0u, position = 0u;

    while (position < length && count < out_capacity) {
        if (text[position] == '[') {
            size_t close_label = position + 1u;
            while (close_label < length && text[close_label] != ']') ++close_label;
            if (close_label + 2u < length && text[close_label + 1u] == '(') {
                size_t close_url = close_label + 2u;
                while (close_url < length && text[close_url] != ')') ++close_url;
                if (close_url < length) {
                    markdown_push(out, out_capacity, &count, plain_start,
                                  position - plain_start, SOL_MARKDOWN_INLINE_PLAIN);
                    markdown_push(out, out_capacity, &count, position + 1u,
                                  close_label - position - 1u, SOL_MARKDOWN_INLINE_LINK);
                    position = close_url + 1u;
                    plain_start = position;
                    continue;
                }
            }
        }

        SolMarkdownInlineStyle style;
        const size_t marker = markdown_delimiter(text, length, position, &style);
        if (marker == 0u || (position > 0u && text[position - 1u] == '\\')) {
            ++position;
            continue;
        }
        size_t close = position + marker;
        while (close + marker <= length &&
               memcmp(text + close, text + position, marker) != 0) ++close;
        if (close + marker > length || close == position + marker) {
            ++position;
            continue;
        }
        markdown_push(out, out_capacity, &count, plain_start,
                      position - plain_start, SOL_MARKDOWN_INLINE_PLAIN);
        markdown_push(out, out_capacity, &count, position + marker,
                      close - position - marker, style);
        position = close + marker;
        plain_start = position;
    }
    markdown_push(out, out_capacity, &count, plain_start,
                  length - plain_start, SOL_MARKDOWN_INLINE_PLAIN);
    return count;
}
