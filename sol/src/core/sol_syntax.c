// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_syntax.c — Language registry implementation. */

#include "sol_syntax.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Global registry                                                     */
/* ------------------------------------------------------------------ */

static SolSyntaxRegistry *g_global_registry = NULL;

void sol_syntax_set_global_registry(SolSyntaxRegistry *reg)
{
    g_global_registry = reg;
}

SolSyntaxRegistry *sol_syntax_get_global_registry(void)
{
    return g_global_registry;
}

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
    const char *query_text;                            /* highlights.scm or NULL */
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

    /* Update-in-place when the same lang_id is registered again (reload). */
    SolSyntaxEntry *e = NULL;
    for (size_t i = 0u; i < reg->count; i++) {
        if (strcmp(reg->entries[i].lang_id, lang_id) == 0) {
            e = &reg->entries[i];
            break;
        }
    }
    if (!e) {
        if (reg->count >= SOL_SYNTAX_MAX_LANGS) return false;
        e = &reg->entries[reg->count++];
    }

    e->language   = language;
    e->query_text = NULL;  /* caller may set via register_with_query */
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

    return true;
}

void sol_syntax_registry_unregister(SolSyntaxRegistry *reg, const char *lang_id)
{
    if (!reg || !lang_id) return;
    for (size_t i = 0u; i < reg->count; i++) {
        if (strcmp(reg->entries[i].lang_id, lang_id) != 0) continue;
        /* Compact the array by shifting entries down. */
        for (size_t j = i + 1u; j < reg->count; j++)
            reg->entries[j - 1u] = reg->entries[j];
        reg->count--;
        /* A given lang_id should appear at most once; done. */
        return;
    }
}

/* Register with an optional highlights.scm query string pointer.
 * query_text must point to a string that outlives the registry entry
 * (typically a static constant in the plugin). */
bool sol_syntax_registry_register_with_query(
        SolSyntaxRegistry  *reg,
        const char         *lang_id,
        const void         *language,
        const char *const  *extensions,
        const char         *query_text)
{
    if (!sol_syntax_registry_register(reg, lang_id, language, extensions))
        return false;
    /* Find the entry we just inserted/updated and attach the query. */
    for (size_t i = 0u; i < reg->count; i++) {
        if (strcmp(reg->entries[i].lang_id, lang_id) == 0) {
            reg->entries[i].query_text = query_text;
            break;
        }
    }
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

    /* 1. Try extension-based match (e.g. ".c", ".cmake"). */
    const char *dot = strrchr(path, '.');
    if (dot) {
        const void *lang = sol_syntax_get_by_extension(reg, dot);
        if (lang) return lang;
    }

    /* 2. Fall back to full-basename match so filenames like
     *    "CMakeLists.txt" can be registered without clobbering .txt. */
    const char *slash = strrchr(path, '/');
    const char *basename = slash ? slash + 1 : path;
    if (*basename) {
        for (size_t i = 0u; i < reg->count; i++) {
            const SolSyntaxEntry *e = &reg->entries[i];
            for (size_t j = 0u; j < e->ext_count; j++) {
                /* Basename entries have no leading dot. */
                if (e->exts[j][0] != '.') {
                    if (strcmp(e->exts[j], basename) == 0)
                        return e->language;
                }
            }
        }
    }
    return NULL;
}

size_t sol_syntax_registry_count(const SolSyntaxRegistry *reg)
{
    return reg ? reg->count : 0u;
}

/* Return the raw highlights.scm query text for the language matching
 * the given file path, or NULL when none was registered.             */
const char *sol_syntax_get_query_for_path(const SolSyntaxRegistry *reg,
                                           const char              *path)
{
    if (!reg || !path) return NULL;

    const char *dot   = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;

    for (size_t i = 0u; i < reg->count; i++) {
        const SolSyntaxEntry *e = &reg->entries[i];
        for (size_t j = 0u; j < e->ext_count; j++) {
            if (dot   && strcmp(e->exts[j], dot)  == 0) return e->query_text;
            if (*base && e->exts[j][0] != '.' &&
                strcmp(e->exts[j], base) == 0)          return e->query_text;
        }
    }
    return NULL;
}
