// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#include "sol_syntax.h"

SolSyntaxRegistry *sol_syntax_registry_create(void) { return (SolSyntaxRegistry *)1; }
void sol_syntax_registry_destroy(SolSyntaxRegistry *reg) { (void)reg; }

bool sol_syntax_registry_register(SolSyntaxRegistry *reg,
                                  const char *lang_id,
                                  const void *language,
                                  const char *const *extensions)
{
    return reg && lang_id && language && extensions;
}

bool sol_syntax_registry_register_with_query(SolSyntaxRegistry *reg,
                                             const char *lang_id,
                                             const void *language,
                                             const char *const *extensions,
                                             const char *query_text)
{
    (void)query_text;
    return sol_syntax_registry_register(reg, lang_id, language, extensions);
}

void sol_syntax_registry_unregister(SolSyntaxRegistry *reg, const char *lang_id)
{
    (void)reg;
    (void)lang_id;
}

const void *sol_syntax_get_by_extension(const SolSyntaxRegistry *reg,
                                        const char *extension)
{
    (void)reg;
    (void)extension;
    return NULL;
}

const void *sol_syntax_get_for_path(const SolSyntaxRegistry *reg,
                                    const char *path)
{
    (void)reg;
    (void)path;
    return NULL;
}

const char *sol_syntax_get_query_for_path(const SolSyntaxRegistry *reg,
                                          const char *path)
{
    (void)reg;
    (void)path;
    return NULL;
}

size_t sol_syntax_registry_count(const SolSyntaxRegistry *reg)
{
    return reg ? 0u : 0u;
}

void sol_syntax_set_global_registry(SolSyntaxRegistry *reg) { (void)reg; }
SolSyntaxRegistry *sol_syntax_get_global_registry(void) { return NULL; }
