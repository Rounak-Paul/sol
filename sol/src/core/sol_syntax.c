// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_syntax.c — Language registry implementation. */

#include "sol_syntax.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define SOL_SYNTAX_MAX_LANGS 64u
#define SOL_SYNTAX_MAX_EXTS   8u
#define SOL_SYNTAX_MAX_EXT_LEN 16u

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct SolSyntaxEntry {
    const void *language;                              /* TSLanguage* opaque */
    char        lang_id[64];
    char        exts[SOL_SYNTAX_MAX_EXTS][SOL_SYNTAX_MAX_EXT_LEN];
    size_t      ext_count;
} SolSyntaxEntry;

struct SolSyntaxRegistry {
    SolSyntaxEntry entries[SOL_SYNTAX_MAX_LANGS];
    size_t         count;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolSyntaxRegistry *sol_syntax_registry_create(void)
{
    SolSyntaxRegistry *reg =
        (SolSyntaxRegistry *)calloc(1u, sizeof(SolSyntaxRegistry));
    return reg;
}

void sol_syntax_registry_destroy(SolSyntaxRegistry *reg)
{
    free(reg);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

bool sol_syntax_registry_register(SolSyntaxRegistry  *reg,
                                   const char         *lang_id,
                                   const void         *language,
                                   const char *const  *extensions)
{
    if (!reg || !lang_id || !language || !extensions) return false;
    if (reg->count >= SOL_SYNTAX_MAX_LANGS)           return false;

    SolSyntaxEntry *e = &reg->entries[reg->count];
    e->language   = language;
    e->ext_count  = 0u;

    strncpy(e->lang_id, lang_id, sizeof(e->lang_id) - 1u);
    e->lang_id[sizeof(e->lang_id) - 1u] = '\0';

    for (size_t i = 0u; extensions[i] && e->ext_count < SOL_SYNTAX_MAX_EXTS; i++) {
        strncpy(e->exts[e->ext_count],
                extensions[i],
                SOL_SYNTAX_MAX_EXT_LEN - 1u);
        e->exts[e->ext_count][SOL_SYNTAX_MAX_EXT_LEN - 1u] = '\0';
        e->ext_count++;
    }

    reg->count++;
    return true;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

const void *sol_syntax_get_by_extension(const SolSyntaxRegistry *reg,
                                         const char              *extension)
{
    if (!reg || !extension) return NULL;

    for (size_t i = 0u; i < reg->count; i++) {
        const SolSyntaxEntry *e = &reg->entries[i];
        for (size_t j = 0u; j < e->ext_count; j++) {
            if (strcmp(e->exts[j], extension) == 0)
                return e->language;
        }
    }
    return NULL;
}

const void *sol_syntax_get_for_path(const SolSyntaxRegistry *reg,
                                     const char              *path)
{
    if (!reg || !path) return NULL;
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    return sol_syntax_get_by_extension(reg, dot);
}

size_t sol_syntax_registry_count(const SolSyntaxRegistry *reg)
{
    return reg ? reg->count : 0u;
}
