// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_syntax_highlight.c — Tree-sitter based syntax highlighter.
 *
 * When the language plugin supplies a highlights.scm query, the primary
 * path uses the TSQuery API (full pattern matching + predicate evaluation).
 * The legacy DFS node-type walker is kept as an automatic fallback for
 * any buffer whose language has no query registered.
 */

#include "sol_syntax_highlight.h"
#include "sol_rope.h"

#include <tree_sitter/api.h>

#if defined(_WIN32)
#include "sol_regex.h"
#else
#include <regex.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Predicate table                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    PRED_NONE,
    PRED_MATCH,      /* #match?      — regex match on capture text */
    PRED_NOT_MATCH,  /* #not-match?  */
    PRED_EQ,         /* #eq?         — exact string equality */
    PRED_NOT_EQ,     /* #not-eq?     */
} SolPredKind;

/* One predicate per pattern (we evaluate only the first predicate per
 * pattern; chained predicates on the same pattern are uncommon in the
 * grammars shipped with Sol's plugins and are skipped without warning). */
typedef struct SolPred {
    SolPredKind kind;
    uint32_t    capture_idx;   /* which @capture the predicate tests */
    bool        regex_valid;
#if defined(_WIN32)
    SolRegex    regex;
#else
    regex_t     regex;
#endif
    char       *eq_str;        /* owned; used by PRED_EQ / PRED_NOT_EQ */
} SolPred;

/* ------------------------------------------------------------------ */
/* Highlighter struct                                                  */
/* ------------------------------------------------------------------ */

struct SolSyntaxHighlighter {
    TSParser      *parser;
    TSTree        *tree;
    TSQuery       *query;       /* NULL → use legacy DFS walker */
    TSQueryCursor *cursor;      /* reused across reparses */
    const void    *language;

    /* Per-capture CSS class; indexed by capture id.  NULL = uncolored. */
    const char   **capture_css;
    uint32_t       capture_count;

    /* Per-pattern first predicate (may be PRED_NONE). */
    SolPred       *pattern_preds;
    uint32_t       pattern_count;

    /* Sorted span output. */
    SolSyntaxSpan *spans;
    size_t         span_count;
    size_t         span_capacity;
    bool           valid;
};

/* ------------------------------------------------------------------ */
/* CSS class ← capture name                                           */
/* ------------------------------------------------------------------ */

static const char *capture_name_to_css(const char *name)
{
    if (!name || !*name) return NULL;

    /* Most-specific matches first to avoid prefix collisions. */
    if (strcmp(name, "comment") == 0)                       return "hl-comment";

    if (strcmp(name, "string.escape")           == 0 ||
        strcmp(name, "escape")                  == 0 ||
        strcmp(name, "escape_sequence")         == 0)       return "hl-escape";
    if (strcmp(name, "string.special")          == 0 ||
        strcmp(name, "string.regex")            == 0 ||
        strcmp(name, "regex")                   == 0)       return "hl-regex";
    if (strncmp(name, "string", 6) == 0)                    return "hl-string";

    if (strncmp(name, "number", 6) == 0 ||
        strncmp(name, "float",  5) == 0)                    return "hl-number";

    if (strcmp(name, "constant.builtin")        == 0 ||
        strcmp(name, "constant.macro")          == 0)       return "hl-macro";
    if (strncmp(name, "constant", 8) == 0)                  return "hl-constant";

    if (strcmp(name, "variable.builtin")        == 0)       return "hl-keyword";
    if (strcmp(name, "variable.parameter")      == 0 ||
        strcmp(name, "parameter")               == 0)       return "hl-parameter";
    if (strncmp(name, "variable", 8) == 0)                  return NULL; /* plain */

    if (strcmp(name, "function.builtin")        == 0 ||
        strcmp(name, "function.special")        == 0 ||
        strcmp(name, "function.macro")          == 0)       return "hl-function";
    if (strncmp(name, "function", 8) == 0)                  return "hl-function";

    if (strncmp(name, "constructor", 11) == 0)              return "hl-constructor";

    if (strcmp(name, "type.builtin")            == 0 ||
        strcmp(name, "type.definition")         == 0 ||
        strcmp(name, "type.qualifier")          == 0)       return "hl-type";
    if (strncmp(name, "type", 4) == 0)                      return "hl-type";

    if (strcmp(name, "tag.attribute")           == 0)       return "hl-attribute";
    if (strcmp(name, "tag.delimiter")           == 0)       return NULL; /* uncolored */
    if (strncmp(name, "tag", 3) == 0)                       return "hl-tag";

    if (strncmp(name, "attribute", 9) == 0)                 return "hl-attribute";
    if (strncmp(name, "property", 8) == 0)                  return "hl-property";

    if (strncmp(name, "keyword", 7) == 0)                   return "hl-keyword";
    if (strncmp(name, "operator", 8) == 0)                  return "hl-operator";
    if (strncmp(name, "namespace", 9) == 0 ||
        strncmp(name, "module",    6) == 0)                 return "hl-namespace";
    if (strcmp(name, "label") == 0)                         return "hl-label";

    /* Markdown / document text captures (nvim-treesitter naming). */
    if (strcmp(name, "text.title") == 0)                    return "hl-function";
    if (strcmp(name, "text.literal") == 0)                  return "hl-string";
    if (strcmp(name, "text.uri") == 0)                      return "hl-string";
    if (strcmp(name, "text.reference") == 0)                return "hl-property";
    if (strcmp(name, "text.emphasis") == 0)                 return "hl-type";
    if (strcmp(name, "text.strong") == 0)                   return "hl-keyword";

    /* Punctuation, embedded, markup — intentionally uncolored. */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Regex preprocessing: replace tree-sitter \d \w \s with POSIX BRE  */
/* ------------------------------------------------------------------ */

static void preprocess_pattern(const char *src, char *dst, size_t dst_sz)
{
    size_t di = 0u;
    for (size_t si = 0u; src[si] && di + 8u < dst_sz; si++) {
        if (src[si] == '\\' && src[si + 1u]) {
            const char *repl = NULL;
            switch (src[si + 1u]) {
                case 'd': repl = "[0-9]";          break;
                case 'D': repl = "[^0-9]";         break;
                case 'w': repl = "[A-Za-z0-9_]";   break;
                case 'W': repl = "[^A-Za-z0-9_]";  break;
                case 's': repl = "[[:space:]]";     break;
                case 'S': repl = "[^[:space:]]";    break;
                default:  break;
            }
            if (repl) {
                size_t rlen = strlen(repl);
                if (di + rlen + 1u < dst_sz) {
                    memcpy(dst + di, repl, rlen);
                    di += rlen;
                }
                si++; /* skip the letter after backslash */
                continue;
            }
        }
        dst[di++] = src[si];
    }
    dst[di] = '\0';
}

static char *sol_syntax_strdup_len(const char *src, size_t len)
{
    char *out = (char *)malloc(len + 1u);
    if (!out) return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static bool sol_syntax_regex_compile(SolPred *pred, const char *pattern)
{
#if defined(_WIN32)
    return sol_regex_compile(&pred->regex, pattern);
#else
    return regcomp(&pred->regex, pattern, REG_EXTENDED | REG_NOSUB) == 0;
#endif
}

static bool sol_syntax_regex_match(const SolPred *pred, const char *text)
{
#if defined(_WIN32)
    return sol_regex_match(&pred->regex, text);
#else
    return regexec(&pred->regex, text, 0, NULL, 0) == 0;
#endif
}

static void sol_syntax_regex_destroy(SolPred *pred)
{
#if defined(_WIN32)
    sol_regex_destroy(&pred->regex);
#else
    regfree(&pred->regex);
#endif
}

/* ------------------------------------------------------------------ */
/* Build per-pattern predicate table                                  */
/* ------------------------------------------------------------------ */

static void build_preds(struct SolSyntaxHighlighter *h)
{
    for (uint32_t pi = 0u; pi < h->pattern_count; pi++) {
        SolPred *pred = &h->pattern_preds[pi];
        pred->kind = PRED_NONE;

        uint32_t nsteps;
        const TSQueryPredicateStep *steps =
            ts_query_predicates_for_pattern(h->query, pi, &nsteps);

        /* We need at least: op-string, capture, value-string, done = 4 steps. */
        if (nsteps < 3u) continue;
        if (steps[0].type != TSQueryPredicateStepTypeString)  continue;
        if (steps[1].type != TSQueryPredicateStepTypeCapture) continue;
        if (steps[2].type != TSQueryPredicateStepTypeString)  continue;

        uint32_t op_len;
        const char *op = ts_query_string_value_for_id(
            h->query, steps[0].value_id, &op_len);

        bool is_match     = (strcmp(op, "match?")     == 0 ||
                             strcmp(op, "any-match?")  == 0);
        bool is_not_match = (strcmp(op, "not-match?")  == 0);
        bool is_eq        = (strcmp(op, "eq?")         == 0 ||
                             strcmp(op, "any-eq?")     == 0);
        bool is_not_eq    = (strcmp(op, "not-eq?")     == 0);

        if (!is_match && !is_not_match && !is_eq && !is_not_eq) continue;

        uint32_t val_len;
        const char *val = ts_query_string_value_for_id(
            h->query, steps[2].value_id, &val_len);

        pred->capture_idx = steps[1].value_id;

        if (is_match || is_not_match) {
            char processed[512];
            preprocess_pattern(val, processed, sizeof(processed));
            pred->kind        = is_match ? PRED_MATCH : PRED_NOT_MATCH;
            pred->regex_valid = sol_syntax_regex_compile(pred, processed);
        } else {
            pred->kind   = is_eq ? PRED_EQ : PRED_NOT_EQ;
            pred->eq_str = sol_syntax_strdup_len(val, val_len);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Predicate evaluation for a single match                             */
/* ------------------------------------------------------------------ */

static bool eval_pred(const SolPred *pred, const TSQueryMatch *match,
                      const char *src, uint32_t src_len)
{
    if (pred->kind == PRED_NONE) return true;

    /* Find the node for the tested capture. */
    const TSNode *node = NULL;
    for (uint16_t i = 0u; i < match->capture_count; i++) {
        if (match->captures[i].index == pred->capture_idx) {
            node = &match->captures[i].node;
            break;
        }
    }
    if (!node) return false;

    uint32_t start = ts_node_start_byte(*node);
    uint32_t end   = ts_node_end_byte(*node);
    if (start >= src_len || end > src_len || start >= end) return false;

    uint32_t    len = end - start;
    char        stk[256];
    char       *dyn = NULL;
    const char *text;

    if (len < sizeof(stk)) {
        memcpy(stk, src + start, len);
        stk[len] = '\0';
        text = stk;
    } else {
        dyn = (char *)malloc(len + 1u);
        if (!dyn) return false;
        memcpy(dyn, src + start, len);
        dyn[len] = '\0';
        text = dyn;
    }

    bool result;
    switch (pred->kind) {
        case PRED_MATCH:
            result = pred->regex_valid && sol_syntax_regex_match(pred, text);
            break;
        case PRED_NOT_MATCH:
            result = pred->regex_valid && !sol_syntax_regex_match(pred, text);
            break;
        case PRED_EQ:
            result = pred->eq_str && strcmp(text, pred->eq_str) == 0;
            break;
        case PRED_NOT_EQ:
            result = !pred->eq_str || strcmp(text, pred->eq_str) != 0;
            break;
        default:
            result = true;
            break;
    }
    free(dyn);
    return result;
}

/* ------------------------------------------------------------------ */
/* Span management                                                     */
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
        h->spans         = p;
        h->span_capacity = new_cap;
    }
    h->spans[h->span_count++] = (SolSyntaxSpan){
        .start_byte = start,
        .end_byte   = end,
        .css_class  = css,
    };
}

static int span_cmp(const void *a, const void *b)
{
    const SolSyntaxSpan *x = (const SolSyntaxSpan *)a;
    const SolSyntaxSpan *y = (const SolSyntaxSpan *)b;
    if (x->start_byte != y->start_byte)
        return (x->start_byte < y->start_byte) ? -1 : 1;
    /* Longer span first so the outer capture wins for overlaps. */
    if (x->end_byte != y->end_byte)
        return (x->end_byte > y->end_byte) ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Query-based span collection                                         */
/* ------------------------------------------------------------------ */

static void collect_query_spans(struct SolSyntaxHighlighter *h,
                                 const char *src, uint32_t src_len)
{
    ts_query_cursor_exec(h->cursor, h->query,
                         ts_tree_root_node(h->tree));

    TSQueryMatch match;
    while (ts_query_cursor_next_match(h->cursor, &match)) {
        const SolPred *pred = &h->pattern_preds[match.pattern_index];
        if (!eval_pred(pred, &match, src, src_len)) continue;

        for (uint16_t ci = 0u; ci < match.capture_count; ci++) {
            const TSQueryCapture *cap = &match.captures[ci];
            if (cap->index >= h->capture_count) continue;
            const char *css = h->capture_css[cap->index];
            if (!css) continue;
            push_span(h,
                      ts_node_start_byte(cap->node),
                      ts_node_end_byte(cap->node),
                      css);
        }
    }

    /* Sort by position; deduplicate (query cursor may emit overlapping
     * spans from different patterns — keep the first one that "wins"). */
    if (h->span_count > 1u)
        qsort(h->spans, h->span_count, sizeof(SolSyntaxSpan), span_cmp);
}

/* ------------------------------------------------------------------ */
/* Legacy DFS walker (fallback when no highlights.scm query)          */
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

static const char *type_to_css_legacy(const char *type, bool is_named,
                                       const char *parent_type)
{
    if (!type || !*type) return NULL;

    if (!is_named) {
        const char *t = (*type == '#') ? type + 1 : type;
        if (type_is_word(t) && (*t >= 'a' && *t <= 'z'))
            return "hl-keyword";
        return NULL;
    }

    if (strstr(type, "comment") || strcmp(type, "hash_bang_line") == 0)
        return "hl-comment";

    if (strcmp(type, "string")                 == 0 ||
        strcmp(type, "string_literal")         == 0 ||
        strcmp(type, "string_content")         == 0 ||
        strcmp(type, "string_fragment")        == 0 ||
        strcmp(type, "raw_string")             == 0 ||
        strcmp(type, "raw_string_literal")     == 0 ||
        strcmp(type, "template_string")        == 0 ||
        strcmp(type, "char_literal")           == 0 ||
        strcmp(type, "system_lib_string")      == 0 ||
        strcmp(type, "quoted_attribute_value") == 0)
        return "hl-string";

    if (strcmp(type, "escape_sequence") == 0)  return "hl-escape";
    if (strcmp(type, "regex")           == 0)  return "hl-regex";

    if (strstr(type, "number_literal")  ||
        strstr(type, "integer_literal") ||
        strstr(type, "float_literal")   ||
        strcmp(type, "number")   == 0   ||
        strcmp(type, "integer")  == 0   ||
        strcmp(type, "float")    == 0   ||
        strcmp(type, "color_value") == 0)
        return "hl-number";

    if (strcmp(type, "primitive_type")          == 0 ||
        strcmp(type, "type_identifier")         == 0 ||
        strcmp(type, "sized_type_specifier")    == 0 ||
        strcmp(type, "predefined_type")         == 0 ||
        strcmp(type, "builtin_type")            == 0)
        return "hl-type";

    if (strcmp(type, "namespace_identifier") == 0) return "hl-namespace";

    if (strcmp(type, "true")      == 0 || strcmp(type, "false")     == 0 ||
        strcmp(type, "null")      == 0 || strcmp(type, "nullptr")   == 0 ||
        strcmp(type, "None")      == 0 || strcmp(type, "True")      == 0 ||
        strcmp(type, "False")     == 0 || strcmp(type, "undefined") == 0 ||
        strcmp(type, "this")      == 0 || strcmp(type, "self")      == 0)
        return "hl-keyword";

    if (strcmp(type, "tag_name") == 0)             return "hl-tag";
    if (strcmp(type, "attribute_name") == 0)       return "hl-attribute";
    if (strcmp(type, "property_name") == 0)        return "hl-property";
    if (strcmp(type, "field_identifier") == 0)     return "hl-property";
    if (strcmp(type, "statement_identifier") == 0) return "hl-label";

    if (strcmp(type, "identifier") == 0) {
        if (!parent_type) return NULL;
        if (strcmp(parent_type, "function_declarator")  == 0 ||
            strcmp(parent_type, "function_definition")  == 0 ||
            strcmp(parent_type, "method_definition")    == 0 ||
            strcmp(parent_type, "constructor_definition") == 0)
            return "hl-function";
        if (strcmp(parent_type, "call_expression") == 0 ||
            strcmp(parent_type, "call")            == 0 ||
            strcmp(parent_type, "normal_command")  == 0)
            return "hl-function";
        if (strcmp(parent_type, "preproc_def")          == 0 ||
            strcmp(parent_type, "preproc_function_def") == 0)
            return "hl-macro";
        if (strcmp(parent_type, "class_definition")  == 0 ||
            strcmp(parent_type, "class_specifier")   == 0 ||
            strcmp(parent_type, "struct_specifier")  == 0 ||
            strcmp(parent_type, "enum_specifier")    == 0)
            return "hl-type";
        return NULL;
    }
    return NULL;
}

static void collect_spans_legacy(TSNode node, const char *parent_type,
                                  struct SolSyntaxHighlighter *h)
{
    uint32_t child_count = ts_node_child_count(node);
    if (child_count == 0u) {
        const char *css = type_to_css_legacy(
            ts_node_type(node), ts_node_is_named(node), parent_type);
        if (css)
            push_span(h, ts_node_start_byte(node), ts_node_end_byte(node), css);
        return;
    }
    const char *my_type = ts_node_type(node);
    for (uint32_t i = 0u; i < child_count; i++)
        collect_spans_legacy(ts_node_child(node, i), my_type, h);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolSyntaxHighlighter *sol_syntax_highlight_create(const void *language,
                                                   const char *query_text)
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

    /* Compile the highlights.scm query if provided. */
    if (query_text && *query_text) {
        uint32_t err_offset;
        TSQueryError err_type;
        h->query = ts_query_new(
            (const TSLanguage *)language,
            query_text, (uint32_t)strlen(query_text),
            &err_offset, &err_type);

        if (h->query) {
            h->cursor        = ts_query_cursor_new();
            h->capture_count = ts_query_capture_count(h->query);
            h->pattern_count = ts_query_pattern_count(h->query);

            /* Build capture → CSS class lookup. */
            if (h->capture_count > 0u) {
                h->capture_css = (const char **)calloc(
                    h->capture_count, sizeof(const char *));
                if (h->capture_css) {
                    for (uint32_t ci = 0u; ci < h->capture_count; ci++) {
                        uint32_t nlen;
                        const char *name = ts_query_capture_name_for_id(
                            h->query, ci, &nlen);
                        h->capture_css[ci] = capture_name_to_css(name);
                    }
                }
            }

            /* Build per-pattern predicate table. */
            if (h->pattern_count > 0u) {
                h->pattern_preds = (SolPred *)calloc(
                    h->pattern_count, sizeof(SolPred));
                if (h->pattern_preds)
                    build_preds(h);
            }
        }
        /* If compilation failed, fall back to the legacy walker silently. */
    }

    return h;
}

void sol_syntax_highlight_destroy(SolSyntaxHighlighter *h)
{
    if (!h) return;
    if (h->tree)   ts_tree_delete(h->tree);
    if (h->parser) ts_parser_delete(h->parser);
    if (h->cursor) ts_query_cursor_delete(h->cursor);

    if (h->query && h->pattern_preds) {
        for (uint32_t pi = 0u; pi < h->pattern_count; pi++) {
            SolPred *p = &h->pattern_preds[pi];
            if (p->kind == PRED_MATCH || p->kind == PRED_NOT_MATCH)
                if (p->regex_valid) sol_syntax_regex_destroy(p);
            free(p->eq_str);
        }
        free(h->pattern_preds);
    }
    if (h->query) ts_query_delete(h->query);
    free(h->capture_css);
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
    char *buf = (char *)malloc(byte_len + 1u);
    if (!buf) { h->valid = false; return; }
    sol_rope_read(rope, 0u, (uint8_t *)buf, byte_len);
    buf[byte_len] = '\0';

    if (h->tree) { ts_tree_delete(h->tree); h->tree = NULL; }
    h->tree = ts_parser_parse_string(
        h->parser, NULL, buf, (uint32_t)byte_len);

    h->span_count = 0u;
    h->valid      = false;

    if (!h->tree) { free(buf); return; }

    if (h->query && h->cursor && h->capture_css && h->pattern_preds) {
        collect_query_spans(h, buf, (uint32_t)byte_len);
    } else {
        collect_spans_legacy(ts_tree_root_node(h->tree), NULL, h);
    }

    free(buf);
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

