// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_syntax_highlight.c — Tree-sitter based syntax highlighter. */

#include "sol_syntax_highlight.h"
#include "sol_rope.h"

#include <tree_sitter/api.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

struct SolSyntaxHighlighter {
    TSParser      *parser;
    TSTree        *tree;
    const void    *language;    /* TSLanguage* — kept for re-parse safety */

    SolSyntaxSpan *spans;
    size_t         span_count;
    size_t         span_capacity;
    bool           valid;
};

/* ------------------------------------------------------------------ */
/* Node type → CSS class                                               */
/* ------------------------------------------------------------------ */

static bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
            c == '_';
}

static bool type_is_word(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        if (!is_word_char(*p)) return false;
    }
    return true;
}

/* Map a leaf node's type string + named flag to a CSS highlight class,
 * or NULL to leave the segment unstyled (i.e. use the plain color). */
static const char *type_to_css(const char *type, bool is_named)
{
    if (!type || !*type) return NULL;

    if (!is_named) {
        /* Anonymous node — keyword literal or punctuation/operator.
         * Keyword: all word chars (e.g. "if", "return", "struct").
         * Preprocessor: '#' followed by word chars (e.g. "#include").
         * Punctuation / operator: skip (too noisy). */
        const char *t = (*type == '#') ? type + 1 : type;
        if (type_is_word(t) && (*t >= 'a' && *t <= 'z'))
            return "hl-keyword";
        return NULL;
    }

    /* Named nodes — structural tokens worth coloring. */

    /* Comments */
    if (strstr(type, "comment"))
        return "hl-comment";

    /* String content */
    if (strstr(type, "string_fragment") ||
        strstr(type, "string_literal")  ||
        strstr(type, "raw_string")      ||
        strstr(type, "template_string") ||
        strstr(type, "char_literal")    ||
        strstr(type, "byte_literal")    ||
        strcmp(type, "string") == 0)
        return "hl-string";
    /* Escape sequences inside strings */
    if (strcmp(type, "escape_sequence") == 0)
        return "hl-string";

    /* Numbers */
    if (strstr(type, "number_literal")   ||
        strstr(type, "integer_literal")  ||
        strstr(type, "float_literal")    ||
        strstr(type, "decimal_integer")  ||
        strstr(type, "octal_integer")    ||
        strstr(type, "binary_integer")   ||
        strstr(type, "hex_literal")      ||
        strcmp(type, "number") == 0)
        return "hl-number";

    /* Types */
    if (strcmp(type, "primitive_type") == 0          ||
        strcmp(type, "type_identifier") == 0          ||
        strcmp(type, "sized_type_specifier") == 0     ||
        strcmp(type, "abstract_type_specifier") == 0  ||
        strcmp(type, "predefined_type") == 0)
        return "hl-type";

    /* Boolean / null literals (some grammars make these named nodes) */
    if (strcmp(type, "true") == 0      ||
        strcmp(type, "false") == 0     ||
        strcmp(type, "null") == 0      ||
        strcmp(type, "nil") == 0       ||
        strcmp(type, "nullptr") == 0   ||
        strcmp(type, "None") == 0      ||
        strcmp(type, "True") == 0      ||
        strcmp(type, "False") == 0     ||
        strcmp(type, "undefined") == 0)
        return "hl-keyword";

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Span collection (DFS walk of leaf nodes)                           */
/* ------------------------------------------------------------------ */

static void push_span(struct SolSyntaxHighlighter *h,
                      uint32_t start, uint32_t end, const char *css)
{
    if (start >= end) return;
    if (h->span_count >= h->span_capacity) {
        size_t new_cap = h->span_capacity ? h->span_capacity * 2u : 512u;
        SolSyntaxSpan *p = (SolSyntaxSpan *)realloc(
            h->spans, new_cap * sizeof(SolSyntaxSpan));
        if (!p) return;
        h->spans        = p;
        h->span_capacity = new_cap;
    }
    h->spans[h->span_count++] = (SolSyntaxSpan){
        .start_byte = start,
        .end_byte   = end,
        .css_class  = css,
    };
}

static void collect_spans(TSNode node, struct SolSyntaxHighlighter *h)
{
    uint32_t child_count = ts_node_child_count(node);
    if (child_count == 0u) {
        /* Leaf node */
        const char *css = type_to_css(
            ts_node_type(node),
            ts_node_is_named(node));
        if (css) {
            push_span(h,
                      ts_node_start_byte(node),
                      ts_node_end_byte(node),
                      css);
        }
        return;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        collect_spans(ts_node_child(node, i), h);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolSyntaxHighlighter *sol_syntax_highlight_create(const void *language)
{
    if (!language) return NULL;
    SolSyntaxHighlighter *h =
        (SolSyntaxHighlighter *)calloc(1u, sizeof(SolSyntaxHighlighter));
    if (!h) return NULL;

    h->parser = ts_parser_new();
    if (!h->parser) { free(h); return NULL; }

    if (!ts_parser_set_language(h->parser, (const TSLanguage *)language)) {
        ts_parser_delete(h->parser);
        free(h);
        return NULL;
    }

    h->language = language;
    return h;
}

void sol_syntax_highlight_destroy(SolSyntaxHighlighter *h)
{
    if (!h) return;
    if (h->tree)   ts_tree_delete(h->tree);
    if (h->parser) ts_parser_delete(h->parser);
    free(h->spans);
    free(h);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

void sol_syntax_highlight_reparse(SolSyntaxHighlighter *h,
                                   const SolRope        *rope)
{
    if (!h || !rope) return;

    const size_t byte_len = sol_rope_byte_len(rope);

    /* Copy the full document to a flat buffer for ts_parser_parse_string. */
    char *buf = (char *)malloc(byte_len + 1u);
    if (!buf) { h->valid = false; return; }
    sol_rope_read(rope, 0u, (uint8_t *)buf, byte_len);
    buf[byte_len] = '\0';

    /* Delete the previous tree and re-parse from scratch.
     * Incremental re-parsing (ts_tree_edit) can be added later. */
    if (h->tree) {
        ts_tree_delete(h->tree);
        h->tree = NULL;
    }

    h->tree = ts_parser_parse_string(h->parser, NULL, buf, (uint32_t)byte_len);
    free(buf);

    h->span_count = 0u;
    h->valid      = false;

    if (!h->tree) return;

    /* Walk the tree and collect colored leaf spans. */
    collect_spans(ts_tree_root_node(h->tree), h);
    h->valid = true;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

size_t sol_syntax_highlight_spans_for_range(
    const SolSyntaxHighlighter *h,
    uint32_t                    line_start_byte,
    uint32_t                    line_end_byte,
    SolSyntaxSpan              *out,
    size_t                      max_out)
{
    if (!h || !h->valid || !out || max_out == 0u) return 0u;
    if (line_start_byte >= line_end_byte)           return 0u;

    size_t written = 0u;
    for (size_t i = 0u; i < h->span_count && written < max_out; i++) {
        const SolSyntaxSpan *sp = &h->spans[i];
        /* Spans are sorted by start_byte; once past the line we're done. */
        if (sp->start_byte >= line_end_byte) break;
        if (sp->end_byte   <= line_start_byte) continue;
        out[written++] = *sp;
    }
    return written;
}

bool sol_syntax_highlight_is_valid(const SolSyntaxHighlighter *h)
{
    return h && h->valid;
}

const void *sol_syntax_highlight_get_language(const SolSyntaxHighlighter *h)
{
    return h ? h->language : NULL;
}
