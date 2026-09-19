// SPDX-License-Identifier: Apache-2.0

#include "test_harness.h"
#include "sol_markdown.h"

static void test_inline_tokens(SolTestCtx *T)
{
    SolMarkdownInlineToken tokens[12];
    size_t count = sol_markdown_inline_tokens(
        "plain **strong** *em* ~~old~~ `code` [link](https://sol.dev)", tokens, 12u);
    SOL_CHECK_EQ_SZ(T, count, 10u);
    SOL_CHECK_EQ_INT(T, tokens[1].style, SOL_MARKDOWN_INLINE_STRONG);
    SOL_CHECK_EQ_INT(T, tokens[3].style, SOL_MARKDOWN_INLINE_EMPHASIS);
    SOL_CHECK_EQ_INT(T, tokens[5].style, SOL_MARKDOWN_INLINE_STRIKETHROUGH);
    SOL_CHECK_EQ_INT(T, tokens[7].style, SOL_MARKDOWN_INLINE_CODE);
    SOL_CHECK_EQ_INT(T, tokens[9].style, SOL_MARKDOWN_INLINE_LINK);
}

static void test_link_and_unmatched_delimiters(SolTestCtx *T)
{
    SolMarkdownInlineToken tokens[4];
    size_t count = sol_markdown_inline_tokens("[Sol](https://sol.dev)", tokens, 4u);
    SOL_CHECK_EQ_SZ(T, count, 1u);
    SOL_CHECK_EQ_INT(T, tokens[0].style, SOL_MARKDOWN_INLINE_LINK);
    SOL_CHECK_EQ_SZ(T, tokens[0].byte_length, 3u);

    count = sol_markdown_inline_tokens("unfinished **marker", tokens, 4u);
    SOL_CHECK_EQ_SZ(T, count, 1u);
    SOL_CHECK_EQ_INT(T, tokens[0].style, SOL_MARKDOWN_INLINE_PLAIN);
}

static void test_blocks_and_fenced_language(SolTestCtx *T)
{
    SolMarkdownParserState state;
    sol_markdown_parser_init(&state);
    SolMarkdownBlock block = sol_markdown_parse_line("# Heading", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_HEADING);
    SOL_CHECK_EQ_INT(T, block.heading_level, 1);

    block = sol_markdown_parse_line("```mermaid", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_FENCE);
    block = sol_markdown_parse_line("graph TD", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_CODE);
    SOL_CHECK_STR(T, block.fence_language, "mermaid");
    block = sol_markdown_parse_line("```", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_FENCE);

    block = sol_markdown_parse_line("- [x] Done", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_TASK);
    SOL_CHECK(T, block.task_checked);

    block = sol_markdown_parse_line("| Name | Value |", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_TABLE);
    block = sol_markdown_parse_line("| --- | :---: |", &state);
    SOL_CHECK_EQ_INT(T, block.kind, SOL_MARKDOWN_BLOCK_TABLE_SEPARATOR);
}

int main(void)
{
    SolTestSuite s;
    sol_suite_init(&s, "sol_markdown");
    SOL_RUN(s, test_inline_tokens);
    SOL_RUN(s, test_link_and_unmatched_delimiters);
    SOL_RUN(s, test_blocks_and_fenced_language);
    return sol_suite_report(&s);
}
