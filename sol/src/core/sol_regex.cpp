// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_regex.h"

#include <regex>

bool sol_regex_compile(SolRegex *regex, const char *pattern)
{
    if (!regex || !pattern) {
        return false;
    }

    sol_regex_destroy(regex);

    try {
        regex->impl = new std::regex(pattern, std::regex::ECMAScript);
        return true;
    } catch (const std::regex_error &) {
        regex->impl = nullptr;
        return false;
    }
}

bool sol_regex_match(const SolRegex *regex, const char *text)
{
    if (!regex || !regex->impl || !text) {
        return false;
    }

    const std::regex *compiled = static_cast<const std::regex *>(regex->impl);
    return std::regex_search(text, *compiled);
}

void sol_regex_destroy(SolRegex *regex)
{
    if (!regex || !regex->impl) {
        return;
    }

    delete static_cast<std::regex *>(regex->impl);
    regex->impl = nullptr;
}
