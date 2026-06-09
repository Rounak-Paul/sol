// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_syntax.h — Language registry for syntax highlighting.
 *
 * Plugins call sol_plugin_register_language() (in sol_plugin_ctx.h) to
 * associate a tree-sitter TSLanguage* with one or more file extensions.
 * Sol uses the registry to look up the correct parser for a buffer based
 * on the file's extension.
 *
 * TSLanguage pointers are stored as const void* to keep this header free
 * of the tree-sitter dependency; cast to const TSLanguage* at the call
 * sites that actually invoke the tree-sitter runtime API.
 */

#ifndef SOL_SYNTAX_H
#define SOL_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolSyntaxRegistry SolSyntaxRegistry;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Create a new, empty syntax registry. Returns NULL on allocation failure. */
SolSyntaxRegistry *sol_syntax_registry_create(void);

/* Destroy the registry and free all internal resources. */
void               sol_syntax_registry_destroy(SolSyntaxRegistry *reg);

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

/*
 * Register a tree-sitter language for a set of file extensions.
 *
 * reg         The syntax registry.
 * lang_id     Stable dotted identifier, e.g. "sol.lang.cpp".
 * language    const TSLanguage* cast to const void*.
 * extensions  NULL-terminated array of extensions including the dot,
 *             e.g. { ".c", ".h", NULL }. Duplicate extensions are overwritten.
 * Returns     false when the registry is full or an argument is NULL.
 */
bool sol_syntax_registry_register(SolSyntaxRegistry  *reg,
                                   const char         *lang_id,
                                   const void         *language,
                                   const char *const  *extensions);

/*
 * Register a language and store a highlights.scm query string.
 *
 * reg         The syntax registry.
 * lang_id     Stable dotted identifier for the language.
 * language    const TSLanguage* cast to const void*.
 * extensions  NULL-terminated array of file extensions.
 * query_text  Raw highlights.scm text; must remain valid for the registry's lifetime.
 * Returns     false when the registry is full or an argument is NULL.
 */
bool sol_syntax_registry_register_with_query(
                                   SolSyntaxRegistry  *reg,
                                   const char         *lang_id,
                                   const void         *language,
                                   const char *const  *extensions,
                                   const char         *query_text);

/*
 * Remove all entries whose lang_id matches.
 *
 * Must be called BEFORE the library that owns the TSLanguage* is unloaded.
 * No-op when the lang_id is not found.
 *
 * reg      The syntax registry.
 * lang_id  Language identifier to remove.
 */
void sol_syntax_registry_unregister(SolSyntaxRegistry *reg,
                                     const char        *lang_id);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

/*
 * Return the TSLanguage* (as void*) registered for a file extension.
 *
 * reg        The syntax registry.
 * extension  File extension including the dot, e.g. ".c".
 * Returns    Language pointer, or NULL when not registered.
 */
const void *sol_syntax_get_by_extension(const SolSyntaxRegistry *reg,
                                         const char              *extension);

/*
 * Return the TSLanguage* for a file path by deriving its extension.
 *
 * reg   The syntax registry.
 * path  File path; the extension is extracted and used for lookup.
 * Returns  Language pointer, or NULL when the extension is not registered.
 */
const void *sol_syntax_get_for_path(const SolSyntaxRegistry *reg,
                                     const char              *path);

/*
 * Return the highlights.scm query text for the language matched by a file path.
 *
 * reg   The syntax registry.
 * path  File path used to derive the extension and look up the language.
 * Returns  Query text pointer, or NULL when no query was registered.
 */
const char *sol_syntax_get_query_for_path(const SolSyntaxRegistry *reg,
                                           const char              *path);

/* Returns the number of languages currently registered. */
size_t sol_syntax_registry_count(const SolSyntaxRegistry *reg);

/* ================================================================== */
/* Module-level global registry                                        */
/*                                                                     */
/* A single process-wide registry pointer used by the text-buffer     */
/* system to automatically attach a highlighter when a file is opened.*/
/* Set this once during startup before loading plugins.               */
/* ================================================================== */

/*
 * Set the process-wide global syntax registry.
 *
 * Call once during startup before loading plugins.
 *
 * reg  The registry to install as the global instance.
 */
void               sol_syntax_set_global_registry(SolSyntaxRegistry *reg);

/* Returns the process-wide global syntax registry, or NULL if unset. */
SolSyntaxRegistry *sol_syntax_get_global_registry(void);

#ifdef __cplusplus
}
#endif

#endif /* SOL_SYNTAX_H */
