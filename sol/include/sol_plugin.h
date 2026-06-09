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

/*
 * Descriptor that every plugin exports via its sol_plugin_query function.
 *
 * Plugins fill this struct and return true from sol_plugin_query to signal
 * compatibility with the requested API version.
 */
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

/*
 * Entry-point type every dynamic plugin MUST export as "sol_plugin_query".
 *
 * requested_api_version  The API version the manager was compiled against.
 * out_api                Filled with the plugin's descriptor on success.
 * Returns                true if the plugin supports requested_api_version.
 */
typedef bool (*SolPluginQueryFn)(uint32_t requested_api_version,
                                  SolPluginAPI *out_api);

/* ================================================================== */
/* Manager lifecycle                                                   */
/* ================================================================== */

/* Configuration for the plugin manager's directory and initial storage. */
typedef struct SolPluginManagerConfig {
    const char *plugin_directory; /* default "plugins" relative to cwd  */
    size_t      initial_capacity;
} SolPluginManagerConfig;

/* Return a SolPluginManagerConfig populated with sensible defaults. */
SolPluginManagerConfig sol_plugin_manager_config_default(void);

/*
 * Create a new plugin manager.
 *
 * systems  The system manager that provides subsystem access to plugins.
 * config   Directory path and capacity hints.
 * Returns  A heap-allocated manager, or NULL on failure.
 */
SolPluginManager *sol_plugin_manager_create(
    SolSystemManager             *systems,
    const SolPluginManagerConfig *config);

/* Unload all plugins, then destroy the manager and free its resources. */
void sol_plugin_manager_destroy(SolPluginManager *manager);

/*
 * Attach the UI system so plugins can call sol_plugin_ui().
 *
 * Call this after sol_ui_system_create and before loading plugins.
 *
 * manager  The plugin manager.
 * ui       The UI system to attach.
 */
void sol_plugin_manager_attach_ui(SolPluginManager *manager,
                                   SolUISystem      *ui);

/* Attach the syntax registry so plugins can register tree-sitter
 * languages.  Call this before loading plugins.                       */
typedef struct SolSyntaxRegistry SolSyntaxRegistry;

/*
 * Attach the syntax registry so plugins can register tree-sitter languages.
 *
 * Call this before loading plugins.
 *
 * manager   The plugin manager.
 * registry  The syntax registry to attach.
 */
void sol_plugin_manager_attach_syntax_registry(
    SolPluginManager  *manager,
    SolSyntaxRegistry *registry);

/* ================================================================== */
/* Loading                                                             */
/* ================================================================== */

/*
 * Register a statically-linked plugin without opening a shared library.
 *
 * manager  The plugin manager.
 * api      Plugin descriptor; must remain valid for the manager's lifetime.
 * Returns  true on success.
 */
bool sol_plugin_manager_register_static(SolPluginManager   *manager,
                                         const SolPluginAPI *api);

/*
 * Load a single shared-library plugin by path.
 *
 * manager  The plugin manager.
 * path     Path to the .dylib / .so / .dll file.
 * Returns  true if the plugin loaded and on_load returned true.
 */
bool sol_plugin_manager_load(SolPluginManager *manager, const char *path);

/*
 * Load all plugins from a directory in dependency order.
 *
 * manager         The plugin manager.
 * directory_path  Directory to scan, or NULL to use the configured default.
 * Returns         Number of plugins successfully loaded.
 */
size_t sol_plugin_manager_load_directory(SolPluginManager *manager,
                                          const char       *directory_path);

/* ================================================================== */
/* Unloading / reloading                                               */
/* ================================================================== */

/*
 * Unload a single plugin by id.
 *
 * manager    The plugin manager.
 * plugin_id  Dotted id of the plugin to unload.
 * Returns    true if the plugin was found and unloaded.
 */
bool   sol_plugin_manager_unload(SolPluginManager *manager,
                                  const char       *plugin_id);

/*
 * Unload all currently loaded plugins.
 *
 * Returns  Number of plugins unloaded.
 */
size_t sol_plugin_manager_unload_all(SolPluginManager *manager);

/*
 * Reload a dynamic plugin in-place: unload then reload from the same path.
 *
 * Only valid for dynamically-loaded plugins; static plugins cannot be reloaded.
 *
 * manager    The plugin manager.
 * plugin_id  Dotted id of the plugin to reload.
 * Returns    true on success; false if not found, not dynamic, or reload fails.
 */
bool   sol_plugin_manager_reload(SolPluginManager *manager,
                                  const char       *plugin_id);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

/* Returns the total number of plugin records (enabled and disabled). */
size_t sol_plugin_manager_count(SolPluginManager *manager);

/* ================================================================== */
/* Plugin enumeration                                                  */
/* ================================================================== */

/*
 * Read-only snapshot of a plugin's metadata.
 *
 * All string pointers are owned by the manager and remain valid until
 * the next manager mutation (load / unload / disable). Copy them if
 * stability across mutations is needed.
 */
typedef struct SolPluginInfo {
    const char *id;
    const char *display_name;
    const char *version;
    const char *path;        /* NULL for static (built-in) plugins   */
    bool        is_dynamic;
    bool        enabled;
} SolPluginInfo;

/*
 * Fill out_info with the metadata for the plugin record at a given index.
 *
 * Includes both enabled and disabled-but-retained records.
 *
 * manager   The plugin manager.
 * index     Zero-based index in [0, sol_plugin_manager_count()).
 * out_info  Receives the plugin metadata on success.
 * Returns   false when index >= sol_plugin_manager_count().
 */
bool sol_plugin_manager_get_info_at(SolPluginManager *manager,
                                     size_t            index,
                                     SolPluginInfo    *out_info);

/* ================================================================== */
/* Enable / disable                                                    */
/* ================================================================== */

/*
 * Disable a plugin without removing its record from the manager.
 *
 * Calls on_unload and cleans up the ctx, but keeps the record so the
 * plugin can be re-enabled without re-registering. The shared-library
 * handle (for dynamic plugins) remains open.
 *
 * manager    The plugin manager.
 * plugin_id  Dotted id of the plugin to disable.
 * Returns    true if the plugin was found and was enabled; false otherwise.
 */
bool sol_plugin_manager_disable(SolPluginManager *manager,
                                 const char       *plugin_id);

/*
 * Re-enable a previously-disabled plugin.
 *
 * Allocates a fresh ctx and calls on_load.
 *
 * manager    The plugin manager.
 * plugin_id  Dotted id of the plugin to enable.
 * Returns    true on success; false if not found, already enabled, or
 *            on_load returns false.
 */
bool sol_plugin_manager_enable(SolPluginManager *manager,
                                const char       *plugin_id);

#endif /* SOL_PLUGIN_H */
