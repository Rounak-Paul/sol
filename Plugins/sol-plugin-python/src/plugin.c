// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-python — Python language syntax plugin. */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"

typedef struct TSLanguage TSLanguage;
extern const TSLanguage *tree_sitter_python(void);

static bool on_load(SolPluginCtx *ctx)
{
    static const char *const exts[] = { ".py", ".pyw", ".pyi", NULL };
    return sol_plugin_register_language(ctx, tree_sitter_python(), exts);
}

static void on_unload(SolPluginCtx *ctx) { (void)ctx; }

static const SolPluginAPI g_api = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "sol.lang.python",
    .display_name = "Python",
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
