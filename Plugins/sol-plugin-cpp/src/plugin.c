// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-cpp — C++ language syntax plugin.
 * Registers the tree-sitter-cpp grammar for .cpp, .cc, .cxx, .hpp, .h files.
 */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

typedef struct TSLanguage TSLanguage;
extern const TSLanguage *tree_sitter_cpp(void);

static bool on_load(SolPluginCtx *ctx)
{
    static const char *const exts[] = {
        ".cpp", ".cc", ".cxx", ".c++",
        ".hpp", ".hh", ".hxx", ".h++",
        NULL
    };
    return sol_plugin_register_language(ctx, tree_sitter_cpp(), exts);
}

static void on_unload(SolPluginCtx *ctx) { (void)ctx; }

static const SolPluginAPI g_api = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "sol.lang.cpp",
    .display_name = "C++",
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
