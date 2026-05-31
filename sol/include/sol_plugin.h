// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_plugin.h — Plugin manager and plugin descriptor contract.
 *
 * A plugin is a shared library that exports a single symbol:
 *
 *   bool sol_plugin_query(uint32_t api_version, SolPluginAPI *out_api);
 *
 * The query function fills *out_api and returns true if the plugin
 * supports the requested api_version; false to abort loading.
 *
 * The manager calls on_load(ctx) once per plugin load.  The SolPluginCtx
 * handle is the plugin's complete API surface (see sol_plugin_ctx.h).
 * Plugins must not cache system pointers across frames — use the ctx
 * accessors every time.
 *
 * Dependency ordering:
 *   Set `after` to a NULL-terminated list of plugin IDs that must be
 *   loaded before this one.  sol_plugin_manager_load_directory performs
 *   a topological sort and honours this order.  Cycles abort loading of
 *   the affected plugins with an error message.
 */

#ifndef SOL_PLUGIN_H
#define SOL_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SolSystemManager SolSystemManager;
typedef struct SolPluginManager SolPluginManager;
typedef struct SolPluginCtx     SolPluginCtx;
typedef struct SolUISystem      SolUISystem;

/* Bump this when the SolPluginCtx API gains breaking changes. */
#define SOL_PLUGIN_API_VERSION 2u

typedef struct SolPluginAPI {
    uint32_t    api_version;     /* Must equal SOL_PLUGIN_API_VERSION   */
    const char *id;              /* Unique dotted ID, e.g. "com.co.foo" */
    const char *display_name;    /* Human-readable name                  */
    const char *version;         /* Semver string, e.g. "1.0.0"         */

    /* NULL-terminated list of plugin IDs that must load before this one.
     * At most 7 dependencies; remaining entries must be NULL.           */
    const char *after[8];

    /* on_load  — called once after the plugin is successfully opened.
     *            Return false to abort loading (the plugin is closed).  */
    bool (*on_load)(SolPluginCtx *ctx);

    /* on_unload — called before the plugin's shared library is closed.
     *             All resources tracked by the ctx are freed after this. */
    void (*on_unload)(SolPluginCtx *ctx);
} SolPluginAPI;

/* Every dynamic plugin MUST export a symbol of this type named
 * "sol_plugin_query".                                                   */
typedef bool (*SolPluginQueryFn)(uint32_t requested_api_version,
                                  SolPluginAPI *out_api);

/* ================================================================== */
/* Manager lifecycle                                                   */
/* ================================================================== */

typedef struct SolPluginManagerConfig {
    const char *plugin_directory; /* default "plugins" relative to cwd  */
    size_t      initial_capacity;
} SolPluginManagerConfig;

SolPluginManagerConfig sol_plugin_manager_config_default(void);

SolPluginManager *sol_plugin_manager_create(
    SolSystemManager             *systems,
    const SolPluginManagerConfig *config);

void sol_plugin_manager_destroy(SolPluginManager *manager);

/* Attach the UI system so plugins can call sol_plugin_ui().
 * Call this after sol_ui_system_create and before loading plugins.    */
void sol_plugin_manager_attach_ui(SolPluginManager *manager,
                                   SolUISystem      *ui);

/* ================================================================== */
/* Loading                                                             */
/* ================================================================== */

/* Register a statically-linked plugin.  The SolPluginAPI struct must
 * remain valid for the plugin manager's lifetime.                      */
bool sol_plugin_manager_register_static(SolPluginManager   *manager,
                                         const SolPluginAPI *api);

/* Load a single .dylib/.so plugin by path.                            */
bool sol_plugin_manager_load(SolPluginManager *manager, const char *path);

/* Load all plugins from `directory_path` (or the default plugin
 * directory when NULL) in dependency order.  Returns the count loaded. */
size_t sol_plugin_manager_load_directory(SolPluginManager *manager,
                                          const char       *directory_path);

/* ================================================================== */
/* Unloading / reloading                                               */
/* ================================================================== */

bool   sol_plugin_manager_unload(SolPluginManager *manager,
                                  const char       *plugin_id);
size_t sol_plugin_manager_unload_all(SolPluginManager *manager);

/* Reload a plugin in-place: unload, then reload from the same path.
 * The plugin_id must be a dynamically-loaded plugin (not static).
 * Returns false if the plugin was not found or reload fails.           */
bool   sol_plugin_manager_reload(SolPluginManager *manager,
                                  const char       *plugin_id);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

size_t sol_plugin_manager_count(SolPluginManager *manager);

#endif /* SOL_PLUGIN_H */
