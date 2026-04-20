#include "sol_plugin.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SolPluginRecord {
    SolPluginAPI api;
    char *id_owned;
    char *display_name_owned;
    char *version_owned;
    char *path;
    bool is_dynamic;
    void *library_handle;
    void *plugin_state;
} SolPluginRecord;

struct SolPluginManager {
    pthread_mutex_t lock;
    SolSystemManager *systems;

    SolPluginRecord *plugins;
    size_t plugin_count;
    size_t plugin_capacity;

    char *default_directory;
};

static char *sol_strdup(const char *value)
{
    if (!value) {
        return NULL;
    }

    const size_t len = strlen(value);
    char *out = (char *)malloc(len + 1u);
    if (!out) {
        return NULL;
    }

    memcpy(out, value, len + 1u);
    return out;
}

static bool sol_plugin_reserve(SolPluginManager *manager, size_t min_capacity)
{
    if (manager->plugin_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = manager->plugin_capacity == 0u ? 8u : manager->plugin_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolPluginRecord *new_items = (SolPluginRecord *)realloc(
        manager->plugins,
        new_capacity * sizeof(SolPluginRecord)
    );

    if (!new_items) {
        return false;
    }

    manager->plugins = new_items;
    manager->plugin_capacity = new_capacity;
    return true;
}

static bool sol_plugin_has_id_locked(const SolPluginManager *manager, const char *id)
{
    for (size_t i = 0u; i < manager->plugin_count; ++i) {
        if (strcmp(manager->plugins[i].api.id, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool sol_plugin_append(SolPluginManager *manager, SolPluginRecord *record)
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

static bool sol_plugin_extension_matches(const char *name)
{
    if (!name) {
        return false;
    }

    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }

#if defined(__APPLE__)
    return strcmp(dot, ".dylib") == 0;
#elif defined(_WIN32)
    return strcmp(dot, ".dll") == 0;
#else
    return strcmp(dot, ".so") == 0;
#endif
}

static char *sol_plugin_join_path(const char *directory, const char *file_name)
{
    if (!directory || !file_name) {
        return NULL;
    }

    const size_t dir_len = strlen(directory);
    const size_t file_len = strlen(file_name);
    const bool needs_separator = dir_len > 0u && directory[dir_len - 1u] != '/';

    const size_t out_len = dir_len + (needs_separator ? 1u : 0u) + file_len;
    char *path = (char *)malloc(out_len + 1u);
    if (!path) {
        return NULL;
    }

    memcpy(path, directory, dir_len);
    size_t cursor = dir_len;

    if (needs_separator) {
        path[cursor++] = '/';
    }

    memcpy(path + cursor, file_name, file_len);
    path[out_len] = '\0';
    return path;
}

static void sol_plugin_record_deinit(SolPluginManager *manager, SolPluginRecord *record)
{
    if (!record) {
        return;
    }

    if (record->api.on_unload) {
        record->api.on_unload(manager->systems, record->plugin_state);
    }

    if (record->is_dynamic && record->library_handle) {
        dlclose(record->library_handle);
    }

    free(record->id_owned);
    free(record->display_name_owned);
    free(record->version_owned);
    free(record->path);

    memset(record, 0, sizeof(*record));
}

SolPluginManagerConfig sol_plugin_manager_config_default(void)
{
    SolPluginManagerConfig config;
    config.plugin_directory = "plugins";
    config.initial_capacity = 8u;
    return config;
}

SolPluginManager *sol_plugin_manager_create(
    SolSystemManager *systems,
    const SolPluginManagerConfig *config
)
{
    SolPluginManagerConfig effective = config ? *config : sol_plugin_manager_config_default();
    if (effective.initial_capacity == 0u) {
        effective.initial_capacity = 8u;
    }

    SolPluginManager *manager = (SolPluginManager *)calloc(1u, sizeof(SolPluginManager));
    if (!manager) {
        return NULL;
    }

    if (pthread_mutex_init(&manager->lock, NULL) != 0) {
        free(manager);
        return NULL;
    }

    manager->systems = systems;
    manager->plugins = (SolPluginRecord *)calloc(effective.initial_capacity, sizeof(SolPluginRecord));
    manager->plugin_capacity = effective.initial_capacity;
    manager->default_directory = sol_strdup(effective.plugin_directory);

    if (!manager->plugins) {
        free(manager->default_directory);
        pthread_mutex_destroy(&manager->lock);
        free(manager);
        return NULL;
    }

    return manager;
}

void sol_plugin_manager_destroy(SolPluginManager *manager)
{
    if (!manager) {
        return;
    }

    sol_plugin_manager_unload_all(manager);
    free(manager->plugins);
    free(manager->default_directory);
    pthread_mutex_destroy(&manager->lock);
    free(manager);
}

bool sol_plugin_manager_register_static(
    SolPluginManager *manager,
    const SolPluginAPI *api
)
{
    if (!manager || !api || !api->id || api->api_version != SOL_PLUGIN_API_VERSION) {
        return false;
    }

    SolPluginRecord record;
    memset(&record, 0, sizeof(record));

    record.id_owned = sol_strdup(api->id);
    record.display_name_owned = sol_strdup(api->display_name ? api->display_name : api->id);
    record.version_owned = sol_strdup(api->version ? api->version : "0.0.0");

    if (!record.id_owned || !record.display_name_owned || !record.version_owned) {
        free(record.id_owned);
        free(record.display_name_owned);
        free(record.version_owned);
        return false;
    }

    record.api = *api;
    record.api.id = record.id_owned;
    record.api.display_name = record.display_name_owned;
    record.api.version = record.version_owned;
    record.is_dynamic = false;

    if (record.api.on_load && !record.api.on_load(manager->systems, &record.plugin_state)) {
        free(record.id_owned);
        free(record.display_name_owned);
        free(record.version_owned);
        return false;
    }

    if (!sol_plugin_append(manager, &record)) {
        if (record.api.on_unload) {
            record.api.on_unload(manager->systems, record.plugin_state);
        }

        free(record.id_owned);
        free(record.display_name_owned);
        free(record.version_owned);
        return false;
    }

    return true;
}

bool sol_plugin_manager_load(SolPluginManager *manager, const char *path)
{
    if (!manager || !path || *path == '\0') {
        return false;
    }

    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "sol_plugin: failed to load %s: %s\n", path, dlerror());
        return false;
    }

    dlerror();
    SolPluginQueryFn query = (SolPluginQueryFn)dlsym(library, "sol_plugin_query");
    const char *symbol_error = dlerror();
    if (symbol_error != NULL || !query) {
        fprintf(stderr, "sol_plugin: missing sol_plugin_query in %s\n", path);
        dlclose(library);
        return false;
    }

    SolPluginAPI api;
    memset(&api, 0, sizeof(api));

    if (!query(SOL_PLUGIN_API_VERSION, &api)) {
        fprintf(stderr, "sol_plugin: %s rejected API version %u\n", path, SOL_PLUGIN_API_VERSION);
        dlclose(library);
        return false;
    }

    if (api.api_version != SOL_PLUGIN_API_VERSION || !api.id) {
        fprintf(stderr, "sol_plugin: %s returned invalid API descriptor\n", path);
        dlclose(library);
        return false;
    }

    SolPluginRecord record;
    memset(&record, 0, sizeof(record));

    record.id_owned = sol_strdup(api.id);
    record.display_name_owned = sol_strdup(api.display_name ? api.display_name : api.id);
    record.version_owned = sol_strdup(api.version ? api.version : "0.0.0");
    record.path = sol_strdup(path);
    record.library_handle = library;
    record.is_dynamic = true;

    if (!record.id_owned || !record.display_name_owned || !record.version_owned || !record.path) {
        free(record.id_owned);
        free(record.display_name_owned);
        free(record.version_owned);
        free(record.path);
        dlclose(library);
        return false;
    }

    record.api = api;
    record.api.id = record.id_owned;
    record.api.display_name = record.display_name_owned;
    record.api.version = record.version_owned;

    if (record.api.on_load && !record.api.on_load(manager->systems, &record.plugin_state)) {
        fprintf(stderr, "sol_plugin: on_load failed for %s\n", path);
        free(record.id_owned);
        free(record.display_name_owned);
        free(record.version_owned);
        free(record.path);
        dlclose(library);
        return false;
    }

    if (!sol_plugin_append(manager, &record)) {
        if (record.api.on_unload) {
            record.api.on_unload(manager->systems, record.plugin_state);
        }

        fprintf(stderr, "sol_plugin: duplicate or capacity failure for id '%s'\n", api.id);
        free(record.id_owned);
        free(record.display_name_owned);
        free(record.version_owned);
        free(record.path);
        dlclose(library);
        return false;
    }

    return true;
}

size_t sol_plugin_manager_load_directory(SolPluginManager *manager, const char *directory_path)
{
    if (!manager) {
        return 0u;
    }

    const char *directory = directory_path ? directory_path : manager->default_directory;
    if (!directory || *directory == '\0') {
        return 0u;
    }

    DIR *dir = opendir(directory);
    if (!dir) {
        return 0u;
    }

    size_t loaded = 0u;
    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        if (!sol_plugin_extension_matches(entry->d_name)) {
            continue;
        }

        char *full_path = sol_plugin_join_path(directory, entry->d_name);
        if (!full_path) {
            continue;
        }

        if (sol_plugin_manager_load(manager, full_path)) {
            ++loaded;
        }

        free(full_path);
    }

    closedir(dir);
    return loaded;
}

bool sol_plugin_manager_unload(SolPluginManager *manager, const char *plugin_id)
{
    if (!manager || !plugin_id || *plugin_id == '\0') {
        return false;
    }

    SolPluginRecord record;
    memset(&record, 0, sizeof(record));

    bool found = false;

    pthread_mutex_lock(&manager->lock);
    for (size_t i = 0u; i < manager->plugin_count; ++i) {
        if (strcmp(manager->plugins[i].api.id, plugin_id) != 0) {
            continue;
        }

        record = manager->plugins[i];

        for (size_t j = i + 1u; j < manager->plugin_count; ++j) {
            manager->plugins[j - 1u] = manager->plugins[j];
        }

        --manager->plugin_count;
        found = true;
        break;
    }
    pthread_mutex_unlock(&manager->lock);

    if (!found) {
        return false;
    }

    sol_plugin_record_deinit(manager, &record);
    return true;
}

size_t sol_plugin_manager_unload_all(SolPluginManager *manager)
{
    if (!manager) {
        return 0u;
    }

    size_t unloaded = 0u;

    for (;;) {
        char *id_to_unload = NULL;

        pthread_mutex_lock(&manager->lock);
        if (manager->plugin_count > 0u) {
            id_to_unload = sol_strdup(manager->plugins[manager->plugin_count - 1u].api.id);
        }
        pthread_mutex_unlock(&manager->lock);

        if (!id_to_unload) {
            break;
        }

        if (sol_plugin_manager_unload(manager, id_to_unload)) {
            ++unloaded;
        }

        free(id_to_unload);
    }

    return unloaded;
}

size_t sol_plugin_manager_count(SolPluginManager *manager)
{
    if (!manager) {
        return 0u;
    }

    pthread_mutex_lock(&manager->lock);
    const size_t count = manager->plugin_count;
    pthread_mutex_unlock(&manager->lock);
    return count;
}
