#ifndef SOL_PLUGIN_H
#define SOL_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SolSystemManager SolSystemManager;
typedef struct SolPluginManager SolPluginManager;

#define SOL_PLUGIN_API_VERSION 1u

typedef struct SolPluginAPI {
    uint32_t api_version;
    const char *id;
    const char *display_name;
    const char *version;
    bool (*on_load)(SolSystemManager *systems, void **plugin_state);
    void (*on_unload)(SolSystemManager *systems, void *plugin_state);
} SolPluginAPI;

typedef bool (*SolPluginQueryFn)(uint32_t requested_api_version, SolPluginAPI *out_api);

typedef struct SolPluginManagerConfig {
    const char *plugin_directory;
    size_t initial_capacity;
} SolPluginManagerConfig;

SolPluginManagerConfig sol_plugin_manager_config_default(void);

SolPluginManager *sol_plugin_manager_create(
    SolSystemManager *systems,
    const SolPluginManagerConfig *config
);

void sol_plugin_manager_destroy(SolPluginManager *manager);

bool sol_plugin_manager_register_static(
    SolPluginManager *manager,
    const SolPluginAPI *api
);

bool sol_plugin_manager_load(SolPluginManager *manager, const char *path);
size_t sol_plugin_manager_load_directory(SolPluginManager *manager, const char *directory_path);

bool sol_plugin_manager_unload(SolPluginManager *manager, const char *plugin_id);
size_t sol_plugin_manager_unload_all(SolPluginManager *manager);

size_t sol_plugin_manager_count(SolPluginManager *manager);

#endif
