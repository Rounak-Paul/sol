// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_syntax_highlight.h"

SolSyntaxHighlighter *sol_syntax_highlight_create(const void *language,
                                                  const char *query_text)
{
    (void)language;
    (void)query_text;
    return NULL;
}

void sol_syntax_highlight_destroy(SolSyntaxHighlighter *h) { (void)h; }

void sol_syntax_highlight_reparse(SolSyntaxHighlighter *h, const SolRope *rope)
{
    (void)h;
    (void)rope;
}

size_t sol_syntax_highlight_spans_for_range(const SolSyntaxHighlighter *h,
                                            uint32_t line_start_byte,
                                            uint32_t line_end_byte,
                                            SolSyntaxSpan *out,
                                            size_t max_out)
{
    (void)h;
    (void)line_start_byte;
    (void)line_end_byte;
    (void)out;
    (void)max_out;
    return 0u;
}

bool sol_syntax_highlight_is_valid(const SolSyntaxHighlighter *h)
{
    (void)h;
    return false;
}

const void *sol_syntax_highlight_get_language(const SolSyntaxHighlighter *h)
{
    (void)h;
    return NULL;
}
