// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

/* sol_plugin.c — Plugin manager + SolPluginCtx implementation.
 *
 * Design notes:
 *
 *   SolPluginCtx is the per-plugin API handle allocated when a plugin
 *   is loaded.  It tracks all resources a plugin registers so they can
 *   be released automatically in on_unload cleanup:
 *     - event subscriptions
 *     - key bindings (sol_input)
 *     - command flows (sol_ui)
 *     - status bar segments (sol_ui)
 *     - service names (for unregistration)
 *
 *   Dependency ordering (after[]):
 *     sol_plugin_manager_load_directory collects all candidate .dylib
 *     files, queries them (without calling on_load), topologically sorts
 *     by after[], and then calls on_load in order.  Cycles are detected
 *     by a simple DFS colouring pass and result in the affected plugins
 *     being skipped with an error to stderr.
 */

#include "sol_plugin.h"
#include "sol_plugin_ctx.h"
#include "sol_platform.h"
#include "sol_syntax.h"
#include "sol_system_manager.h"
#include "sol_threading.h"
#include "sol_text_buffer.h"
#include "sol_ui_system.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define SOL_PLUGIN_CTX_MAX_SUBSCRIPTIONS  64u
#define SOL_PLUGIN_CTX_MAX_BINDINGS       64u
#define SOL_PLUGIN_CTX_MAX_COMMANDS       64u
#define SOL_PLUGIN_CTX_MAX_SERVICES       16u
#define SOL_PLUGIN_CTX_MAX_STATUS_SEGS     8u
#define SOL_PLUGIN_CTX_MAX_LANGUAGES       8u
/* Keep in sync with SOL_UI_MAX_ACTION_LEN in sol_ui_internal.h */
#define SOL_PLUGIN_CMD_ACTION_MAX_LEN     63u

/* ================================================================== */
/* SolPluginCtx                                                        */
/* ================================================================== */

struct SolPluginCtx {
    struct SolPluginManager *manager;   /* back-pointer; never NULL */
    char *id;                           /* owned copy for logging   */
    char *display_name;
    char *version;

    /* Tracked subscriptions — auto-unsubscribed on cleanup */
    SolSubscriptionToken  subs[SOL_PLUGIN_CTX_MAX_SUBSCRIPTIONS];
    size_t                sub_count;

    /* Tracked input bindings — auto-unbound on cleanup */
    SolInputActionToken   bindings[SOL_PLUGIN_CTX_MAX_BINDINGS];
    size_t                binding_count;

    /* Tracked command flow actions — auto-unregistered on cleanup */
    char commands[SOL_PLUGIN_CTX_MAX_COMMANDS][SOL_PLUGIN_CMD_ACTION_MAX_LEN + 1u];
    size_t command_count;

    /* Tracked service names — auto-unregistered on cleanup */
    char services[SOL_PLUGIN_CTX_MAX_SERVICES][128u];
    size_t service_count;

    /* Tracked status bar tokens — auto-removed on cleanup */
    SolUIStatusToken status_tokens[SOL_PLUGIN_CTX_MAX_STATUS_SEGS];
    size_t           status_token_count;

    /* Tracked language registrations — unregistered and buffer highlighters
     * invalidated before the plugin library is unloaded (dlclose). */
    const void *language_ptrs[SOL_PLUGIN_CTX_MAX_LANGUAGES];
    size_t      language_count;
};

/* ================================================================== */
/* SolPluginRecord                                                     */
/* ================================================================== */

typedef struct SolPluginRecord {
    SolPluginAPI     api;
    char            *id_owned;
    char            *display_name_owned;
    char            *version_owned;
    char            *path;            /* NULL for static plugins */
    bool             is_dynamic;
    bool             enabled;         /* false = ctx is NULL, record kept for re-enable */
    void            *library_handle;  /* dlopen handle; NULL for static */
    SolPluginCtx    *ctx;             /* heap-allocated; freed on deinit */
} SolPluginRecord;

/* ================================================================== */
/* SolPluginManager                                                    */
/* ================================================================== */

struct SolPluginManager {
    pthread_mutex_t    lock;
    SolSystemManager  *systems;
    SolUISystem       *ui;               /* set via sol_plugin_manager_attach_ui */
    SolSyntaxRegistry *syntax_registry;  /* set via sol_plugin_manager_attach_syntax_registry */

    SolPluginRecord   *plugins;
    size_t             plugin_count;
    size_t             plugin_capacity;

    char              *default_directory;
};

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

static char *sol_pstrdup(const char *s)
{
    if (!s) return NULL;
    const size_t n = strlen(s);
    char *o = (char *)malloc(n + 1u);
    if (!o) return NULL;
    memcpy(o, s, n + 1u);
    return o;
}

static bool sol_plugin_reserve(SolPluginManager *manager, size_t min_cap)
{
    if (manager->plugin_capacity >= min_cap) return true;
    size_t cap = manager->plugin_capacity ? manager->plugin_capacity : 8u;
    while (cap < min_cap) cap *= 2u;
    SolPluginRecord *arr = (SolPluginRecord *)realloc(
        manager->plugins, cap * sizeof(SolPluginRecord));
    if (!arr) return false;
    manager->plugins = arr;
    manager->plugin_capacity = cap;
    return true;
}

static bool sol_plugin_extension_matches(const char *name)
{
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcmp(dot, sol_platform_dynamic_library_extension()) == 0;
}

static bool sol_plugin_has_id_locked(const SolPluginManager *m, const char *id)
{
    for (size_t i = 0u; i < m->plugin_count; ++i)
        if (strcmp(m->plugins[i].api.id, id) == 0) return true;
    return false;
}

/* ---- SolPluginCtx lifecycle -------------------------------------- */

static SolPluginCtx *plugin_ctx_alloc(SolPluginManager *manager,
                                       const char *id,
                                       const char *display_name,
                                       const char *version)
{
    SolPluginCtx *ctx = (SolPluginCtx *)calloc(1u, sizeof(SolPluginCtx));
    if (!ctx) return NULL;
    ctx->manager      = manager;
    ctx->id           = sol_pstrdup(id);
    ctx->display_name = sol_pstrdup(display_name ? display_name : id);
    ctx->version      = sol_pstrdup(version      ? version      : "0.0.0");
    if (!ctx->id || !ctx->display_name || !ctx->version) {
        free(ctx->id); free(ctx->display_name); free(ctx->version);
        free(ctx);
        return NULL;
    }
    return ctx;
}

/* Release all tracked resources, then free the ctx. */
static void plugin_ctx_cleanup(SolPluginCtx *ctx)
{
    if (!ctx) return;
    SolPluginManager *mgr = ctx->manager;

    /* Unsubscribe events */
    SolEventBus *bus = sol_system_events(mgr->systems);
    if (bus) {
        for (size_t i = 0u; i < ctx->sub_count; ++i)
            sol_event_bus_unsubscribe(bus, ctx->subs[i]);
    }

    /* Unbind keys */
    SolInputSystem *inp = sol_system_input(mgr->systems);
    if (inp) {
        for (size_t i = 0u; i < ctx->binding_count; ++i)
            sol_input_unbind_action(inp, ctx->bindings[i]);
    }

    /* Unregister commands and status segments */
    SolUISystem *ui = mgr->ui;
    if (ui) {
        for (size_t i = 0u; i < ctx->command_count; ++i)
            sol_ui_system_unregister_command_flow(ui, ctx->commands[i]);
        for (size_t i = 0u; i < ctx->status_token_count; ++i)
            sol_ui_system_remove_status_segment(ui, ctx->status_tokens[i]);
    }

    /* Unregister services */
    for (size_t i = 0u; i < ctx->service_count; ++i)
        sol_system_unregister_service(mgr->systems, ctx->services[i]);

    /* Invalidate syntax highlighters in open buffers and remove language
     * entries from the registry.  This must happen BEFORE the plugin
     * library is closed (dlclose) so that the TSLanguage* pointers are
     * still valid when we compare them against highlighter->language. */
    SolSyntaxRegistry *reg = mgr->syntax_registry;
    SolBufferSystem   *bs  = sol_system_buffers(mgr->systems);
    for (size_t i = 0u; i < ctx->language_count; ++i) {
        if (bs)  sol_text_buffer_invalidate_language(bs, ctx->language_ptrs[i]);
        if (reg) sol_syntax_registry_unregister(reg, ctx->id);
    }

    free(ctx->id);
    free(ctx->display_name);
    free(ctx->version);
    free(ctx);
}

/* ---- Record deinit ----------------------------------------------- */

static void sol_plugin_record_deinit(SolPluginManager *manager,
                                      SolPluginRecord  *record)
{
    if (!record) return;

    if (record->api.on_unload && record->ctx)
        record->api.on_unload(record->ctx);

    plugin_ctx_cleanup(record->ctx);
    record->ctx = NULL;

    if (record->is_dynamic && record->library_handle)
        sol_platform_library_close(record->library_handle);

    free(record->id_owned);
    free(record->display_name_owned);
    free(record->version_owned);
    free(record->path);

    (void)manager;
    memset(record, 0, sizeof(*record));
}

/* Append a fully-initialised record (locks). */
static bool sol_plugin_append(SolPluginManager *manager,
                               SolPluginRecord  *record)
{
    pthread_mutex_lock(&manager->lock);
    bool ok = false;
    if (!sol_plugin_has_id_locked(manager, record->api.id) &&
        sol_plugin_reserve(manager, manager->plugin_count + 1u)) {
        manager->plugins[manager->plugin_count++] = *record;
        ok = true;
    }
    pthread_mutex_unlock(&manager->lock);
    return ok;
}

/* Build a record from a SolPluginAPI and optional library handle,
 * allocate a ctx, call on_load.  Returns true on success. */
static bool sol_plugin_finish_load(SolPluginManager *manager,
                                    SolPluginAPI     *api,
                                    void             *library_handle,
                                    bool              is_dynamic,
                                    const char       *path)
{
    SolPluginRecord record;
    memset(&record, 0, sizeof(record));

    record.id_owned           = sol_pstrdup(api->id);
    record.display_name_owned = sol_pstrdup(api->display_name
                                            ? api->display_name : api->id);
    record.version_owned      = sol_pstrdup(api->version
                                            ? api->version : "0.0.0");
    record.path               = sol_pstrdup(path);
    record.is_dynamic         = is_dynamic;
    record.enabled            = true;
    record.library_handle     = library_handle;

    if (!record.id_owned || !record.display_name_owned || !record.version_owned
        || (path && !record.path)) {
        free(record.id_owned); free(record.display_name_owned);
        free(record.version_owned); free(record.path);
        if (is_dynamic && library_handle)
            sol_platform_library_close(library_handle);
        return false;
    }

    record.api              = *api;
    record.api.id           = record.id_owned;
    record.api.display_name = record.display_name_owned;
    record.api.version      = record.version_owned;

    record.ctx = plugin_ctx_alloc(manager,
                                   record.id_owned,
                                   record.display_name_owned,
                                   record.version_owned);
    if (!record.ctx) {
        free(record.id_owned); free(record.display_name_owned);
        free(record.version_owned); free(record.path);
        if (is_dynamic && library_handle)
            sol_platform_library_close(library_handle);
        return false;
    }

    if (api->on_load && !api->on_load(record.ctx)) {
        fprintf(stderr, "sol_plugin: on_load failed for '%s'\n",
                record.id_owned);
        plugin_ctx_cleanup(record.ctx);
        record.ctx = NULL;
        free(record.id_owned); free(record.display_name_owned);
        free(record.version_owned); free(record.path);
        if (is_dynamic && library_handle)
            sol_platform_library_close(library_handle);
        return false;
    }

    if (!sol_plugin_append(manager, &record)) {
        fprintf(stderr, "sol_plugin: duplicate or capacity failure for '%s'\n",
                record.id_owned);
        if (api->on_unload) api->on_unload(record.ctx);
        plugin_ctx_cleanup(record.ctx);
        record.ctx = NULL;
        free(record.id_owned); free(record.display_name_owned);
        free(record.version_owned); free(record.path);
        if (is_dynamic && library_handle)
            sol_platform_library_close(library_handle);
        return false;
    }

    /* Re-attach syntax highlighters to any already-open buffers whose
     * file extension now has a registered language.  Covers the case
     * where a language plugin is loaded (or reloaded) after files are
     * already open in the editor. */
    SolBufferSystem *bs_refresh = sol_system_buffers(manager->systems);
    if (bs_refresh)
        sol_text_buffer_refresh_highlighters(bs_refresh);

    return true;
}

/* ================================================================== */
/* Manager lifecycle                                                   */
/* ================================================================== */

SolPluginManagerConfig sol_plugin_manager_config_default(void)
{
    SolPluginManagerConfig cfg;
    cfg.plugin_directory = "plugins";
    cfg.initial_capacity = 8u;
    return cfg;
}

SolPluginManager *sol_plugin_manager_create(
    SolSystemManager             *systems,
    const SolPluginManagerConfig *config)
{
    SolPluginManagerConfig eff = config
        ? *config : sol_plugin_manager_config_default();
    if (eff.initial_capacity == 0u) eff.initial_capacity = 8u;

    SolPluginManager *m = (SolPluginManager *)calloc(1u, sizeof(*m));
    if (!m) return NULL;

    if (pthread_mutex_init(&m->lock, NULL) != 0) { free(m); return NULL; }

    m->systems           = systems;
    m->default_directory = sol_pstrdup(eff.plugin_directory);
    m->plugins = (SolPluginRecord *)calloc(
        eff.initial_capacity, sizeof(SolPluginRecord));
    m->plugin_capacity = eff.initial_capacity;

    if (!m->plugins) {
        free(m->default_directory);
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }
    return m;
}

void sol_plugin_manager_destroy(SolPluginManager *manager)
{
    if (!manager) return;
    sol_plugin_manager_unload_all(manager);
    free(manager->plugins);
    free(manager->default_directory);
    pthread_mutex_destroy(&manager->lock);
    free(manager);
}

void sol_plugin_manager_attach_ui(SolPluginManager *manager, SolUISystem *ui)
{
    if (manager) manager->ui = ui;
}

void sol_plugin_manager_attach_syntax_registry(SolPluginManager  *manager,
                                                SolSyntaxRegistry *registry)
{
    if (manager) manager->syntax_registry = registry;
}

/* ================================================================== */
/* Loading — single plugin                                             */
/* ================================================================== */

bool sol_plugin_manager_register_static(SolPluginManager   *manager,
                                         const SolPluginAPI *api)
{
    if (!manager || !api || !api->id
        || api->api_version != SOL_PLUGIN_API_VERSION) return false;
    SolPluginAPI copy = *api;
    return sol_plugin_finish_load(manager, &copy, NULL, false, NULL);
}

bool sol_plugin_manager_load(SolPluginManager *manager, const char *path)
{
    if (!manager || !path || !*path) return false;

    void *lib = sol_platform_library_open(path);
    if (!lib) {
        fprintf(stderr, "sol_plugin: failed to open '%s': %s\n",
                path, sol_platform_library_last_error());
        return false;
    }

    SolPluginQueryFn query = (SolPluginQueryFn)
        sol_platform_library_symbol(lib, "sol_plugin_query");
    if (!query) {
        fprintf(stderr, "sol_plugin: missing sol_plugin_query in '%s'\n", path);
        sol_platform_library_close(lib);
        return false;
    }

    SolPluginAPI api;
    memset(&api, 0, sizeof(api));
    if (!query(SOL_PLUGIN_API_VERSION, &api)) {
        fprintf(stderr, "sol_plugin: '%s' rejected API version %u\n",
                path, SOL_PLUGIN_API_VERSION);
        sol_platform_library_close(lib);
        return false;
    }
    if (api.api_version != SOL_PLUGIN_API_VERSION || !api.id) {
        fprintf(stderr, "sol_plugin: '%s' returned invalid descriptor\n", path);
        sol_platform_library_close(lib);
        return false;
    }

    return sol_plugin_finish_load(manager, &api, lib, true, path);
}

/* ================================================================== */
/* Loading — directory (dependency-ordered)                            */
/* ================================================================== */

/* Candidate plugin collected before on_load. */
typedef struct SolPluginCandidate {
    SolPluginAPI  api;
    void         *library_handle;
    char         *path;
    uint8_t       colour;   /* 0=white, 1=grey, 2=black (DFS) */
} SolPluginCandidate;

/* DFS topological sort. Returns false on cycle. */
static bool topo_visit(SolPluginCandidate *cands, size_t ncands,
                        int idx, int *order, size_t *order_count)
{
    SolPluginCandidate *c = &cands[idx];
    if (c->colour == 2u) return true;
    if (c->colour == 1u) {
        fprintf(stderr, "sol_plugin: dependency cycle involving '%s'\n",
                c->api.id ? c->api.id : "?");
        return false;
    }
    c->colour = 1u;

    for (int d = 0; d < 8 && c->api.after[d]; ++d) {
        const char *dep_id = c->api.after[d];
        bool found = false;
        for (size_t j = 0u; j < ncands; ++j) {
            if (cands[j].api.id && strcmp(cands[j].api.id, dep_id) == 0) {
                found = true;
                if (!topo_visit(cands, ncands, (int)j, order, order_count))
                    return false;
                break;
            }
        }
        if (!found) {
            fprintf(stderr,
                    "sol_plugin: dependency '%s' of '%s' not found; loading anyway\n",
                    dep_id, c->api.id ? c->api.id : "?");
        }
    }

    c->colour = 2u;
    order[(*order_count)++] = idx;
    return true;
}

size_t sol_plugin_manager_load_directory(SolPluginManager *manager,
                                          const char       *directory_path)
{
    if (!manager) return 0u;
    const char *dir = directory_path ? directory_path : manager->default_directory;
    if (!dir || !*dir) return 0u;

    /* ---- Phase 1: collect candidates ---- */
    SolPluginCandidate *cands = NULL;
    size_t ncands = 0u, cap = 0u;

    SolDirectoryIter iter;
    if (!sol_platform_dir_open(&iter, dir)) return 0u;

    SolDirectoryEntry entry;
    while (sol_platform_dir_next(&iter, &entry)) {
        if (!entry.name || entry.name[0] == '.') continue;
        if (entry.is_directory) continue;
        if (!sol_plugin_extension_matches(entry.name)) continue;

        char *full = sol_platform_path_join(dir, entry.name);
        if (!full) continue;

        void *lib = sol_platform_library_open(full);
        if (!lib) {
            fprintf(stderr, "sol_plugin: failed to open '%s': %s\n",
                    full, sol_platform_library_last_error());
            free(full);
            continue;
        }

        SolPluginQueryFn query = (SolPluginQueryFn)
            sol_platform_library_symbol(lib, "sol_plugin_query");
        if (!query) {
            fprintf(stderr,
                    "sol_plugin: missing sol_plugin_query in '%s'\n", full);
            sol_platform_library_close(lib);
            free(full);
            continue;
        }

        SolPluginAPI api;
        memset(&api, 0, sizeof(api));
        if (!query(SOL_PLUGIN_API_VERSION, &api)
            || api.api_version != SOL_PLUGIN_API_VERSION || !api.id) {
            fprintf(stderr, "sol_plugin: bad descriptor in '%s'\n", full);
            sol_platform_library_close(lib);
            free(full);
            continue;
        }

        pthread_mutex_lock(&manager->lock);
        bool dup = sol_plugin_has_id_locked(manager, api.id);
        pthread_mutex_unlock(&manager->lock);
        if (dup) {
            sol_platform_library_close(lib);
            free(full);
            continue;
        }

        if (ncands == cap) {
            size_t new_cap = cap ? cap * 2u : 8u;
            SolPluginCandidate *tmp = (SolPluginCandidate *)realloc(
                cands, new_cap * sizeof(*cands));
            if (!tmp) {
                sol_platform_library_close(lib);
                free(full);
                continue;
            }
            cands = tmp;
            cap   = new_cap;
        }

        SolPluginCandidate *c = &cands[ncands];
        memset(c, 0, sizeof(*c));
        c->api            = api;
        c->library_handle = lib;
        c->path           = full;
        ncands++;
    }
    sol_platform_dir_close(&iter);

    if (ncands == 0u) { free(cands); return 0u; }

    /* ---- Phase 2: topological sort ---- */
    int *order = (int *)malloc(ncands * sizeof(int));
    size_t order_count = 0u;
    if (!order) {
        for (size_t i = 0u; i < ncands; ++i) {
            sol_platform_library_close(cands[i].library_handle);
            free(cands[i].path);
        }
        free(cands);
        return 0u;
    }

    for (size_t i = 0u; i < ncands; ++i)
        if (cands[i].colour == 0u)
            topo_visit(cands, ncands, (int)i, order, &order_count);

    /* ---- Phase 3: call on_load in sorted order ---- */
    size_t loaded = 0u;
    for (size_t i = 0u; i < order_count; ++i) {
        SolPluginCandidate *c = &cands[order[i]];
        if (sol_plugin_finish_load(manager, &c->api,
                                    c->library_handle, true, c->path)) {
            c->library_handle = NULL;   /* ownership transferred */
            ++loaded;
        } else if (c->library_handle) {
            sol_platform_library_close(c->library_handle);
            c->library_handle = NULL;
        }
        free(c->path);
        c->path = NULL;
    }

    /* Clean up any candidates not reached by the sort (e.g. cycle victims) */
    for (size_t i = 0u; i < ncands; ++i) {
        if (cands[i].library_handle)
            sol_platform_library_close(cands[i].library_handle);
        free(cands[i].path);
    }

    free(order);
    free(cands);
    return loaded;
}

/* ================================================================== */
/* Unloading / reloading                                               */
/* ================================================================== */

bool sol_plugin_manager_unload(SolPluginManager *manager, const char *plugin_id)
{
    if (!manager || !plugin_id || !*plugin_id) return false;

    SolPluginRecord record;
    memset(&record, 0, sizeof(record));
    bool found = false;

    pthread_mutex_lock(&manager->lock);
    for (size_t i = 0u; i < manager->plugin_count; ++i) {
        if (strcmp(manager->plugins[i].api.id, plugin_id) != 0) continue;
        record = manager->plugins[i];
        for (size_t j = i + 1u; j < manager->plugin_count; ++j)
            manager->plugins[j - 1u] = manager->plugins[j];
        --manager->plugin_count;
        found = true;
        break;
    }
    pthread_mutex_unlock(&manager->lock);

    if (!found) return false;
    sol_plugin_record_deinit(manager, &record);
    return true;
}

size_t sol_plugin_manager_unload_all(SolPluginManager *manager)
{
    if (!manager) return 0u;
    size_t unloaded = 0u;
    for (;;) {
        char *id = NULL;
        pthread_mutex_lock(&manager->lock);
        if (manager->plugin_count > 0u)
            id = sol_pstrdup(manager->plugins[manager->plugin_count - 1u].api.id);
        pthread_mutex_unlock(&manager->lock);
        if (!id) break;
        if (sol_plugin_manager_unload(manager, id)) ++unloaded;
        free(id);
    }
    return unloaded;
}

bool sol_plugin_manager_reload(SolPluginManager *manager, const char *plugin_id)
{
    if (!manager || !plugin_id || !*plugin_id) return false;

    char *saved_path = NULL;
    pthread_mutex_lock(&manager->lock);
    for (size_t i = 0u; i < manager->plugin_count; ++i) {
        if (strcmp(manager->plugins[i].api.id, plugin_id) != 0) continue;
        if (!manager->plugins[i].is_dynamic) {
            pthread_mutex_unlock(&manager->lock);
            fprintf(stderr, "sol_plugin: cannot reload static plugin '%s'\n",
                    plugin_id);
            return false;
        }
        saved_path = sol_pstrdup(manager->plugins[i].path);
        break;
    }
    pthread_mutex_unlock(&manager->lock);

    if (!saved_path) {
        fprintf(stderr, "sol_plugin: plugin '%s' not found for reload\n",
                plugin_id);
        return false;
    }

    sol_plugin_manager_unload(manager, plugin_id);
    const bool ok = sol_plugin_manager_load(manager, saved_path);
    free(saved_path);
    return ok;
}

size_t sol_plugin_manager_count(SolPluginManager *manager)
{
    if (!manager) return 0u;
    pthread_mutex_lock(&manager->lock);
    const size_t n = manager->plugin_count;
    pthread_mutex_unlock(&manager->lock);
    return n;
}

bool sol_plugin_manager_get_info_at(SolPluginManager *manager,
                                     size_t            index,
                                     SolPluginInfo    *out_info)
{
    if (!manager || !out_info) return false;
    pthread_mutex_lock(&manager->lock);
    if (index >= manager->plugin_count) {
        pthread_mutex_unlock(&manager->lock);
        return false;
    }
    const SolPluginRecord *r = &manager->plugins[index];
    out_info->id           = r->api.id;
    out_info->display_name = r->api.display_name;
    out_info->version      = r->api.version;
    out_info->path         = r->path;
    out_info->is_dynamic   = r->is_dynamic;
    out_info->enabled      = r->enabled;
    pthread_mutex_unlock(&manager->lock);
    return true;
}

bool sol_plugin_manager_disable(SolPluginManager *manager, const char *plugin_id)
{
    if (!manager || !plugin_id || !*plugin_id) return false;

    pthread_mutex_lock(&manager->lock);
    SolPluginRecord *record = NULL;
    for (size_t i = 0u; i < manager->plugin_count; ++i) {
        if (strcmp(manager->plugins[i].api.id, plugin_id) == 0) {
            record = &manager->plugins[i];
            break;
        }
    }
    if (!record || !record->enabled) {
        pthread_mutex_unlock(&manager->lock);
        return false;
    }
    /* Mark as disabled while still holding the lock, then clean up
       ctx outside the lock (user on_unload code must not lock). */
    SolPluginCtx *ctx = record->ctx;
    record->ctx     = NULL;
    record->enabled = false;
    pthread_mutex_unlock(&manager->lock);

    if (record->api.on_unload && ctx)
        record->api.on_unload(ctx);
    plugin_ctx_cleanup(ctx);
    return true;
}

bool sol_plugin_manager_enable(SolPluginManager *manager, const char *plugin_id)
{
    if (!manager || !plugin_id || !*plugin_id) return false;

    pthread_mutex_lock(&manager->lock);
    SolPluginRecord *record = NULL;
    for (size_t i = 0u; i < manager->plugin_count; ++i) {
        if (strcmp(manager->plugins[i].api.id, plugin_id) == 0) {
            record = &manager->plugins[i];
            break;
        }
    }
    if (!record || record->enabled) {
        pthread_mutex_unlock(&manager->lock);
        return false;
    }
    SolPluginCtx *ctx = plugin_ctx_alloc(manager,
                                          record->id_owned,
                                          record->display_name_owned,
                                          record->version_owned);
    if (!ctx) {
        pthread_mutex_unlock(&manager->lock);
        return false;
    }
    record->ctx     = ctx;
    record->enabled = true;
    pthread_mutex_unlock(&manager->lock);

    if (record->api.on_load && !record->api.on_load(ctx)) {
        pthread_mutex_lock(&manager->lock);
        record->ctx     = NULL;
        record->enabled = false;
        pthread_mutex_unlock(&manager->lock);
        plugin_ctx_cleanup(ctx);
        return false;
    }

    /* Re-attach syntax highlighters to any already-open buffers. */
    SolBufferSystem *bs_en = sol_system_buffers(manager->systems);
    if (bs_en)
        sol_text_buffer_refresh_highlighters(bs_en);

    return true;
}

/* ================================================================== */
/* SolPluginCtx — subsystem accessors                                  */
/* ================================================================== */

SolSystemManager *sol_plugin_systems(SolPluginCtx *ctx)
{
    return ctx ? ctx->manager->systems : NULL;
}

SolEventBus *sol_plugin_event_bus(SolPluginCtx *ctx)
{
    return ctx ? sol_system_events(ctx->manager->systems) : NULL;
}

SolBufferSystem *sol_plugin_buffers(SolPluginCtx *ctx)
{
    return ctx ? sol_system_buffers(ctx->manager->systems) : NULL;
}

SolJobSystem *sol_plugin_jobs(SolPluginCtx *ctx)
{
    return ctx ? sol_system_jobs(ctx->manager->systems) : NULL;
}

SolInputSystem *sol_plugin_input(SolPluginCtx *ctx)
{
    return ctx ? sol_system_input(ctx->manager->systems) : NULL;
}

SolUISystem *sol_plugin_ui(SolPluginCtx *ctx)
{
    if (!ctx) return NULL;
    if (ctx->manager->ui) return ctx->manager->ui;
    return (SolUISystem *)sol_system_get_service(ctx->manager->systems, "sol.ui");
}

/* ================================================================== */
/* SolPluginCtx — metadata                                             */
/* ================================================================== */

const char *sol_plugin_id(const SolPluginCtx *ctx)
{
    return ctx ? ctx->id : NULL;
}

const char *sol_plugin_display_name(const SolPluginCtx *ctx)
{
    return ctx ? ctx->display_name : NULL;
}

const char *sol_plugin_version(const SolPluginCtx *ctx)
{
    return ctx ? ctx->version : NULL;
}

void sol_plugin_log(SolPluginCtx *ctx, const char *fmt, ...)
{
    if (!ctx || !fmt) return;
    fprintf(stderr, "[plugin:%s] ", ctx->id ? ctx->id : "?");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ================================================================== */
/* SolPluginCtx — event subscriptions                                  */
/* ================================================================== */

SolSubscriptionToken sol_plugin_subscribe(SolPluginCtx    *ctx,
                                           const char      *event_name,
                                           SolEventHandler  handler,
                                           void            *user_data)
{
    if (!ctx || !event_name || !handler) return 0u;
    if (ctx->sub_count >= SOL_PLUGIN_CTX_MAX_SUBSCRIPTIONS) {
        fprintf(stderr, "[plugin:%s] subscription limit reached\n", ctx->id);
        return 0u;
    }
    SolEventBus *bus = sol_system_events(ctx->manager->systems);
    if (!bus) return 0u;

    SolSubscriptionToken tok = sol_event_bus_subscribe(bus,
        &(SolEventSubscriptionDesc){
            .event_name = event_name,
            .handler    = handler,
            .user_data  = user_data,
        });
    if (tok) ctx->subs[ctx->sub_count++] = tok;
    return tok;
}

void sol_plugin_unsubscribe(SolPluginCtx *ctx, SolSubscriptionToken token)
{
    if (!ctx || !token) return;
    SolEventBus *bus = sol_system_events(ctx->manager->systems);
    if (bus) sol_event_bus_unsubscribe(bus, token);
    for (size_t i = 0u; i < ctx->sub_count; ++i) {
        if (ctx->subs[i] == token) {
            ctx->subs[i] = ctx->subs[--ctx->sub_count];
            break;
        }
    }
}

/* ================================================================== */
/* SolPluginCtx — command registration                                 */
/* ================================================================== */

bool sol_plugin_register_command(SolPluginCtx               *ctx,
                                   const SolPluginCommandDesc *desc)
{
    if (!ctx || !desc || !desc->action || !desc->callback) return false;
    if (ctx->command_count >= SOL_PLUGIN_CTX_MAX_COMMANDS) {
        fprintf(stderr, "[plugin:%s] command limit reached\n", ctx->id);
        return false;
    }
    SolUISystem *ui = sol_plugin_ui(ctx);
    if (!ui) {
        fprintf(stderr, "[plugin:%s] UI not available for command '%s'\n",
                ctx->id, desc->action);
        return false;
    }

    bool ok = sol_ui_system_register_command_flow(ui,
        &(SolCommandFlowDesc){
            .action          = desc->action,
            .label           = desc->label ? desc->label : desc->action,
            .sequence        = desc->chord,
            .step_modifiers  = desc->chord_mods,
            .sequence_length = desc->chord_length,
            .callback        = desc->callback,
            .user_data       = desc->user_data,
        });    if (!ok) return false;

    snprintf(ctx->commands[ctx->command_count],
             SOL_PLUGIN_CMD_ACTION_MAX_LEN + 1u, "%s", desc->action);
    ctx->command_count++;
    return true;
}

void sol_plugin_unregister_command(SolPluginCtx *ctx, const char *action)
{
    if (!ctx || !action) return;
    SolUISystem *ui = sol_plugin_ui(ctx);
    if (ui) sol_ui_system_unregister_command_flow(ui, action);
    for (size_t i = 0u; i < ctx->command_count; ++i) {
        if (strcmp(ctx->commands[i], action) == 0) {
            if (i < ctx->command_count - 1u)
                memcpy(ctx->commands[i],
                       ctx->commands[ctx->command_count - 1u],
                       SOL_PLUGIN_CMD_ACTION_MAX_LEN + 1u);
            ctx->command_count--;
            break;
        }
    }
}

/* ================================================================== */
/* SolPluginCtx — key binding                                          */
/* ================================================================== */

SolInputActionToken sol_plugin_bind_key(SolPluginCtx              *ctx,
                                         const SolInputBindingDesc *desc)
{
    if (!ctx || !desc) return 0u;
    if (ctx->binding_count >= SOL_PLUGIN_CTX_MAX_BINDINGS) {
        fprintf(stderr, "[plugin:%s] binding limit reached\n", ctx->id);
        return 0u;
    }
    SolInputSystem *inp = sol_system_input(ctx->manager->systems);
    if (!inp) return 0u;

    SolInputActionToken tok = sol_input_bind_action(inp, desc);
    if (tok) ctx->bindings[ctx->binding_count++] = tok;
    return tok;
}

void sol_plugin_unbind_key(SolPluginCtx *ctx, SolInputActionToken token)
{
    if (!ctx || !token) return;
    SolInputSystem *inp = sol_system_input(ctx->manager->systems);
    if (inp) sol_input_unbind_action(inp, token);
    for (size_t i = 0u; i < ctx->binding_count; ++i) {
        if (ctx->bindings[i] == token) {
            ctx->bindings[i] = ctx->bindings[--ctx->binding_count];
            break;
        }
    }
}

/* ================================================================== */
/* SolPluginCtx — status bar segments                                  */
/* ================================================================== */

SolPluginStatusToken sol_plugin_add_status_segment(SolPluginCtx *ctx,
                                                    const char   *text,
                                                    const char   *style_class)
{
    if (!ctx) return SOL_PLUGIN_STATUS_TOKEN_INVALID;
    if (ctx->status_token_count >= SOL_PLUGIN_CTX_MAX_STATUS_SEGS) {
        fprintf(stderr, "[plugin:%s] status segment limit reached\n", ctx->id);
        return SOL_PLUGIN_STATUS_TOKEN_INVALID;
    }
    SolUISystem *ui = sol_plugin_ui(ctx);
    if (!ui) return SOL_PLUGIN_STATUS_TOKEN_INVALID;

    SolUIStatusToken tok = sol_ui_system_add_status_segment(
        ui, text ? text : "", style_class);
    if (tok == SOL_UI_STATUS_TOKEN_INVALID) return SOL_PLUGIN_STATUS_TOKEN_INVALID;
    ctx->status_tokens[ctx->status_token_count++] = tok;
    return (SolPluginStatusToken)tok;
}

void sol_plugin_update_status_segment(SolPluginCtx        *ctx,
                                       SolPluginStatusToken token,
                                       const char          *text)
{
    if (!ctx || token == SOL_PLUGIN_STATUS_TOKEN_INVALID) return;
    SolUISystem *ui = sol_plugin_ui(ctx);
    if (ui) sol_ui_system_update_status_segment(ui, (SolUIStatusToken)token,
                                                 text ? text : "");
}

void sol_plugin_remove_status_segment(SolPluginCtx        *ctx,
                                       SolPluginStatusToken token)
{
    if (!ctx || token == SOL_PLUGIN_STATUS_TOKEN_INVALID) return;
    SolUISystem *ui = sol_plugin_ui(ctx);
    if (ui) sol_ui_system_remove_status_segment(ui, (SolUIStatusToken)token);
    for (size_t i = 0u; i < ctx->status_token_count; ++i) {
        if (ctx->status_tokens[i] == (SolUIStatusToken)token) {
            ctx->status_tokens[i] = ctx->status_tokens[--ctx->status_token_count];
            break;
        }
    }
}

/* ================================================================== */
/* SolPluginCtx — buffer operations                                    */
/* ================================================================== */

SolBufferId sol_plugin_open_file(SolPluginCtx *ctx, const char *path)
{
    if (!ctx || !path) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return 0u;
    const SolBufferId existing = sol_text_buffer_find_by_path(bs, path);
    if (existing) return existing;
    const char *err = NULL;
    return sol_text_buffer_open_file(bs, path, NULL, NULL, &err);
}

SolBufferId sol_plugin_open_scratch(SolPluginCtx *ctx,
                                     const char   *name,
                                     const char   *initial_text,
                                     size_t        initial_len,
                                     const char   *source_path)
{
    if (!ctx) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return 0u;
    return sol_text_buffer_open_string(bs, name ? name : "scratch",
                                        initial_text, initial_len,
                                        source_path, NULL);
}

SolBufferId sol_plugin_open_custom(SolPluginCtx *ctx,
                                    const char   *name,
                                    void         *state,
                                    SolBufferOps  ops)
{
    if (!ctx) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return 0u;
    return sol_buffer_create(bs, &(SolBufferDesc){
        .name  = name ? name : "plugin-buffer",
        .kind  = SOL_BUFFER_KIND_PLUGIN,
        .state = state,
        .ops   = ops,
    });
}

bool sol_plugin_focus_buffer(SolPluginCtx *ctx, SolBufferId id)
{
    if (!ctx || !id) return false;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    return bs ? sol_buffer_set_active_leaf_buffer(bs, id) : false;
}

SolBufferId sol_plugin_active_buffer(SolPluginCtx *ctx)
{
    if (!ctx) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    return bs ? sol_buffer_active_buffer(bs) : 0u;
}

bool sol_plugin_buf_insert(SolPluginCtx *ctx, SolBufferId id,
                            size_t byte_offset,
                            const char *text, size_t len)
{
    if (!ctx || !id || !text || len == 0u) return false;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return false;
    SolBuffer *buf = sol_buffer_get(bs, id);
    if (!buf) return false;
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    if (!tb) return false;
    return sol_text_buffer_insert_bytes(tb, byte_offset, text, len);
}

bool sol_plugin_buf_delete(SolPluginCtx *ctx, SolBufferId id,
                            size_t byte_offset, size_t byte_count)
{
    if (!ctx || !id || byte_count == 0u) return false;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return false;
    SolBuffer *buf = sol_buffer_get(bs, id);
    if (!buf) return false;
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    if (!tb) return false;
    return sol_text_buffer_delete_bytes(tb, byte_offset, byte_count);
}

size_t sol_plugin_buf_read(SolPluginCtx *ctx, SolBufferId id,
                            size_t byte_offset,
                            char *out, size_t out_size)
{
    if (!ctx || !id || !out || out_size < 2u) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return 0u;
    SolBuffer *buf = sol_buffer_get(bs, id);
    if (!buf) return 0u;
    SolRope *rope = sol_text_buffer_rope(buf);
    if (!rope) return 0u;
    size_t n = sol_rope_read(rope, byte_offset,
                              (uint8_t *)out, out_size - 1u);
    out[n] = '\0';
    return n;
}

size_t sol_plugin_buf_length(SolPluginCtx *ctx, SolBufferId id)
{
    if (!ctx || !id) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return 0u;
    SolBuffer *buf = sol_buffer_get(bs, id);
    if (!buf) return 0u;
    SolRope *rope = sol_text_buffer_rope(buf);
    return rope ? sol_rope_byte_len(rope) : 0u;
}

size_t sol_plugin_buf_cursor(SolPluginCtx *ctx, SolBufferId id)
{
    if (!ctx || !id) return 0u;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return 0u;
    SolBuffer *buf = sol_buffer_get(bs, id);
    if (!buf) return 0u;
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    return tb ? sol_text_buffer_cursor_byte(tb) : 0u;
}

bool sol_plugin_buf_set_cursor(SolPluginCtx *ctx, SolBufferId id,
                                size_t byte_offset)
{
    if (!ctx || !id) return false;
    SolBufferSystem *bs = sol_system_buffers(ctx->manager->systems);
    if (!bs) return false;
    SolBuffer *buf = sol_buffer_get(bs, id);
    if (!buf) return false;
    SolTextBuffer *tb = sol_text_buffer_state(buf);
    if (!tb) return false;
    sol_text_buffer_set_cursor_byte(tb, byte_offset);
    return true;
}

/* ================================================================== */
/* SolPluginCtx — async jobs                                           */
/* ================================================================== */

bool sol_plugin_submit_job(SolPluginCtx *ctx,
                            SolJobFn      fn,
                            void         *user_data,
                            SolJobFence  *fence)
{
    if (!ctx || !fn) return false;
    SolJobSystem *jobs = sol_system_jobs(ctx->manager->systems);
    return jobs ? sol_job_system_submit(jobs, fn, user_data, fence) : false;
}

/* ================================================================== */
/* SolPluginCtx — versioned service registry                           */
/* ================================================================== */

bool sol_plugin_register_service(SolPluginCtx       *ctx,
                                   const char         *name,
                                   uint32_t            version,
                                   void               *service,
                                   SolServiceDestroyFn destroy_fn,
                                   void               *destroy_user_data)
{
    if (!ctx || !name || !*name) return false;
    if (ctx->service_count >= SOL_PLUGIN_CTX_MAX_SERVICES) {
        fprintf(stderr, "[plugin:%s] service registry limit reached\n", ctx->id);
        return false;
    }
    if (!sol_system_register_service_v(ctx->manager->systems,
                                        name, version,
                                        service,
                                        destroy_fn, destroy_user_data)) {
        return false;
    }
    snprintf(ctx->services[ctx->service_count++], 128u, "%s", name);
    return true;
}

void *sol_plugin_get_service(SolPluginCtx *ctx,
                               const char   *name,
                               uint32_t      min_version)
{
    if (!ctx || !name) return NULL;
    return sol_system_get_service_v(ctx->manager->systems, name, min_version);
}

/* ================================================================== */
/* SolPluginCtx — language registration                                */
/* ================================================================== */

bool sol_plugin_register_language(SolPluginCtx      *ctx,
                                   const void        *language,
                                   const char *const *extensions)
{
    if (!ctx || !language || !extensions) return false;
    SolSyntaxRegistry *reg = ctx->manager->syntax_registry;
    if (!reg) {
        sol_plugin_log(ctx, "syntax registry not attached — "
                       "language registration skipped");
        return false;
    }
    if (!sol_syntax_registry_register(reg, ctx->id, language, extensions))
        return false;
    /* Track the language pointer so plugin_ctx_cleanup can invalidate open
     * buffer highlighters before the library is closed. */
    if (ctx->language_count < SOL_PLUGIN_CTX_MAX_LANGUAGES)
        ctx->language_ptrs[ctx->language_count++] = language;
    return true;
}

