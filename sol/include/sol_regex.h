// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#ifndef SOL_REGEX_H
#define SOL_REGEX_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque compiled regular expression. Initialise to zero before first use. */
typedef struct SolRegex {
    void *impl;
} SolRegex;

/*
 * Compile a regular expression pattern into regex.
 *
 * regex    Destination for the compiled expression.
 * pattern  POSIX extended regular expression string.
 * Returns  true on success; false if the pattern is invalid.
 */
bool sol_regex_compile(SolRegex *regex, const char *pattern);

/*
 * Test whether a compiled regex matches the entire text string.
 *
 * regex  A successfully compiled regex.
 * text   Null-terminated string to match against.
 * Returns  true if text matches.
 */
bool sol_regex_match(const SolRegex *regex, const char *text);

/* Release resources held by a compiled regex. Safe to call on a zeroed struct. */
void sol_regex_destroy(SolRegex *regex);

#ifdef __cplusplus
}
#endif

#endif /* SOL_REGEX_H */
