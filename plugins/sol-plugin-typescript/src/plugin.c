// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-typescript — TypeScript / TSX language syntax plugin.
 *
 * tree-sitter-typescript provides two grammars:
 *   tree_sitter_typescript() — for .ts files
 *   tree_sitter_tsx()        — for .tsx files
 */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "highlights_scm.h"

typedef struct TSLanguage TSLanguage;
extern const TSLanguage *tree_sitter_typescript(void);
extern const TSLanguage *tree_sitter_tsx(void);

static bool on_load(SolPluginCtx *ctx)
{
    bool ok = true;
    {
        static const char *const exts[] = { ".ts", NULL };
        ok &= sol_plugin_register_language_with_query(ctx, tree_sitter_typescript(), exts, k_highlights_scm);
    }
    {
        static const char *const exts[] = { ".tsx", NULL };
        ok &= sol_plugin_register_language_with_query(ctx, tree_sitter_tsx(), exts, k_highlights_scm);
    }
    return ok;
}

static void on_unload(SolPluginCtx *ctx) { (void)ctx; }

static const SolPluginAPI g_api = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "sol.lang.typescript",
    .display_name = "TypeScript",
    .version      = "1.0.0",
    .on_load      = on_load,
    .on_unload    = on_unload,
};

bool sol_plugin_query(uint32_t version, SolPluginAPI *out)
{
    if (version != SOL_PLUGIN_API_VERSION) return false;
    *out = g_api;
    return true;
}
