// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-cmake — CMake language syntax plugin. */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "highlights_scm.h"

typedef struct TSLanguage TSLanguage;
extern const TSLanguage *tree_sitter_cmake(void);

static bool on_load(SolPluginCtx *ctx)
{
    /* CMakeLists.txt uses .txt extension which clashes with plain text,
     * so we only register .cmake here.  Path-based detection for
     * CMakeLists.txt can be added when sol supports filename patterns. */
    static const char *const exts[] = { ".cmake", "CMakeLists.txt", NULL };
    return sol_plugin_register_language_with_query(ctx, tree_sitter_cmake(), exts, k_highlights_scm);
}

static void on_unload(SolPluginCtx *ctx) { (void)ctx; }

static const SolPluginAPI g_api = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "sol.lang.cmake",
    .display_name = "CMake",
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
