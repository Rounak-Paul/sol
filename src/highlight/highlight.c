/*
 * Sol IDE - Syntax Highlighting
 * Stub implementation - will be expanded later
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Token types for highlighting */
typedef enum {
    SOL_TOKEN_DEFAULT,
    SOL_TOKEN_KEYWORD,
    SOL_TOKEN_TYPE,
    SOL_TOKEN_FUNCTION,
    SOL_TOKEN_STRING,
    SOL_TOKEN_NUMBER,
    SOL_TOKEN_COMMENT,
    SOL_TOKEN_OPERATOR,
    SOL_TOKEN_PREPROCESSOR,
    SOL_TOKEN_CONSTANT,
    SOL_TOKEN_COUNT
} sol_token_type;

/* Highlighter structure (opaque) */
struct sol_highlighter {
    const char* language;
    bool in_multiline_comment;
    bool in_string;
};

/* Public API implementations - stubs */

struct sol_highlighter* sol_highlighter_create(const char* filepath) {
    (void)filepath;
    struct sol_highlighter* hl = calloc(1, sizeof(struct sol_highlighter));
    if (!hl) return NULL;
    hl->language = "text";
    return hl;
}

void sol_highlighter_destroy(struct sol_highlighter* hl) {
    if (hl) free(hl);
}

void sol_highlight_line(struct sol_highlighter* hl, const char* line, size_t len,
                        uint8_t* token_types) {
    (void)hl;
    (void)line;
    /* Mark everything as default token type */
    if (token_types) {
        memset(token_types, SOL_TOKEN_DEFAULT, len);
    }
}

void sol_highlighter_reset(struct sol_highlighter* hl) {
    if (hl) {
        hl->in_multiline_comment = false;
        hl->in_string = false;
    }
}
