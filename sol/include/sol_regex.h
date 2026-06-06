// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#ifndef SOL_REGEX_H
#define SOL_REGEX_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolRegex {
    void *impl;
} SolRegex;

bool sol_regex_compile(SolRegex *regex, const char *pattern);
bool sol_regex_match(const SolRegex *regex, const char *text);
void sol_regex_destroy(SolRegex *regex);

#ifdef __cplusplus
}
#endif

#endif /* SOL_REGEX_H */
