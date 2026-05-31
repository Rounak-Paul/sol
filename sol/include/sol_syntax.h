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

SolSyntaxRegistry *sol_syntax_registry_create(void);
void               sol_syntax_registry_destroy(SolSyntaxRegistry *reg);

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

/* Register a tree-sitter language for the given file extensions.
 *
 * `language`   — a const TSLanguage* cast to const void*.
 * `lang_id`    — stable dotted identifier, e.g. "sol.lang.cpp".
 * `extensions` — NULL-terminated array of extensions incl. dot,
 *                e.g. { ".c", ".h", NULL }.
 *
 * Returns false when the registry is full or an argument is NULL.
 * Duplicate extensions are silently overwritten.                     */
bool sol_syntax_registry_register(SolSyntaxRegistry  *reg,
                                   const char         *lang_id,
                                   const void         *language,
                                   const char *const  *extensions);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

/* Return the TSLanguage* (as void*) for the given extension, or NULL. */
const void *sol_syntax_get_by_extension(const SolSyntaxRegistry *reg,
                                         const char              *extension);

/* Return the TSLanguage* for a file path (derives extension).
 * Returns NULL when the extension is not registered.                */
const void *sol_syntax_get_for_path(const SolSyntaxRegistry *reg,
                                     const char              *path);

/* Return the number of registered languages. */
size_t sol_syntax_registry_count(const SolSyntaxRegistry *reg);

/* ================================================================== */
/* Module-level global registry                                        */
/*                                                                     */
/* A single process-wide registry pointer used by the text-buffer     */
/* system to automatically attach a highlighter when a file is opened.*/
/* Set this once during startup before loading plugins.               */
/* ================================================================== */

void               sol_syntax_set_global_registry(SolSyntaxRegistry *reg);
SolSyntaxRegistry *sol_syntax_get_global_registry(void);

#ifdef __cplusplus
}
#endif

#endif /* SOL_SYNTAX_H */
