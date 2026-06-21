// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol-plugin-json — JSON language syntax plugin. */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "highlights_scm.h"

typedef struct TSLanguage TSLanguage;
extern const TSLanguage *tree_sitter_json(void);

static bool on_load(SolPluginCtx *ctx)
{
    static const char *const exts[] = { ".json", ".jsonc", ".json5", NULL };
    return sol_plugin_register_language_with_query(ctx, tree_sitter_json(), exts, k_highlights_scm);
}

static void on_unload(SolPluginCtx *ctx) { (void)ctx; }

static const SolPluginAPI g_api = {
    .api_version  = SOL_PLUGIN_API_VERSION,
    .id           = "sol.lang.json",
    .display_name = "JSON",
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
