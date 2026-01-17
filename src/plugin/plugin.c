/*
 * Sol IDE - Plugin System
 * Stub implementation for dynamic plugin loading
 */

#include "sol.h"
#include <stdlib.h>
#include <string.h>

/* Plugin manager structure */
struct sol_plugin_manager {
    sol_editor* editor;
    sol_array(sol_plugin*) plugins;
    sol_plugin_api api;
};

/* Plugin system stubs */

sol_plugin_manager* sol_plugin_manager_create(sol_editor* ed) {
    sol_plugin_manager* pm = calloc(1, sizeof(sol_plugin_manager));
    if (!pm) return NULL;
    
    pm->editor = ed;
    pm->plugins = NULL;
    memset(&pm->api, 0, sizeof(sol_plugin_api));
    pm->api.api_version = SOL_PLUGIN_API_VERSION;
    
    return pm;
}

void sol_plugin_manager_destroy(sol_plugin_manager* pm) {
    if (!pm) return;
    
    sol_plugins_shutdown(pm);
    /* TODO: Free plugins array */
    free(pm);
}

sol_result sol_plugin_load(sol_plugin_manager* pm, const char* path) {
    (void)pm;
    (void)path;
    /* TODO: Use dlopen/LoadLibrary to load plugin */
    return SOL_ERROR_UNKNOWN;
}

void sol_plugin_unload(sol_plugin_manager* pm, sol_plugin* plugin) {
    (void)pm;
    if (!plugin) return;
    /* TODO: Use dlclose/FreeLibrary */
    plugin->loaded = false;
    plugin->enabled = false;
}

sol_array(sol_plugin*) sol_plugin_list(sol_plugin_manager* pm) {
    if (!pm) return NULL;
    return pm->plugins;
}

void sol_plugins_shutdown(sol_plugin_manager* pm) {
    if (!pm || !pm->plugins) return;
    
    /* Call shutdown on all plugins */
    for (size_t i = 0; pm->plugins && pm->plugins[i]; i++) {
        sol_plugin* p = pm->plugins[i];
        if (p->loaded && p->info && p->info->shutdown) {
            p->info->shutdown();
        }
        sol_plugin_unload(pm, p);
    }
}
