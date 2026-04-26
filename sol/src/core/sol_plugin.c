#include "sol_plugin.h"

#include "sol_threading.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

static char sol_dl_error_message[512];

static void sol_dl_set_error_from_system(void)
{
    const DWORD err = GetLastError();
    if (err == 0u) {
        sol_dl_error_message[0] = '\0';
        return;
    }

    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        0,
        sol_dl_error_message,
        (DWORD)sizeof(sol_dl_error_message),
        NULL
    );

    if (size == 0u) {
        snprintf(sol_dl_error_message, sizeof(sol_dl_error_message), "Windows error %lu", (unsigned long)err);
    }
}

static void *sol_dlopen(const char *path, int flags)
{
    (void)flags;
    HMODULE module = LoadLibraryA(path);
    if (!module) {
        sol_dl_set_error_from_system();
    } else {
        sol_dl_error_message[0] = '\0';
    }
    return (void *)module;
}

static void *sol_dlsym(void *handle, const char *symbol)
{
    FARPROC proc = GetProcAddress((HMODULE)handle, symbol);
    if (!proc) {
        sol_dl_set_error_from_system();
    } else {
        sol_dl_error_message[0] = '\0';
    }
    return (void *)proc;
}

static int sol_dlclose(void *handle)
{
    if (FreeLibrary((HMODULE)handle)) {
        return 0;
    }

    sol_dl_set_error_from_system();
    return -1;
}

static const char *sol_dlerror(void)
{
    return sol_dl_error_message[0] != '\0' ? sol_dl_error_message : NULL;
}

#define dlopen(path, flags) sol_dlopen(path, flags)
#define dlsym(handle, symbol) sol_dlsym(handle, symbol)
#define dlclose(handle) sol_dlclose(handle)
#define dlerror() sol_dlerror()
#define RTLD_NOW 0
#define RTLD_LOCAL 0

#endif

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

#if defined(_WIN32)
static const char SOL_PATH_SEPARATOR = '\\';
#else
static const char SOL_PATH_SEPARATOR = '/';
#endif

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
    const bool has_separator =
        dir_len > 0u &&
        (directory[dir_len - 1u] == '/' || directory[dir_len - 1u] == '\\');
    const bool needs_separator = dir_len > 0u && !has_separator;

    const size_t out_len = dir_len + (needs_separator ? 1u : 0u) + file_len;
    char *path = (char *)malloc(out_len + 1u);
    if (!path) {
        return NULL;
    }

    memcpy(path, directory, dir_len);
    size_t cursor = dir_len;

    if (needs_separator) {
        path[cursor++] = SOL_PATH_SEPARATOR;
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

    size_t loaded = 0u;

#if defined(_WIN32)
    char *pattern = sol_plugin_join_path(directory, "*");
    if (!pattern) {
        return 0u;
    }

    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = FindFirstFileA(pattern, &find_data);
    free(pattern);

    if (find_handle == INVALID_HANDLE_VALUE) {
        return 0u;
    }

    do {
        if (find_data.cFileName[0] == '.') {
            continue;
        }

        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            continue;
        }

        if (!sol_plugin_extension_matches(find_data.cFileName)) {
            continue;
        }

        char *full_path = sol_plugin_join_path(directory, find_data.cFileName);
        if (!full_path) {
            continue;
        }

        if (sol_plugin_manager_load(manager, full_path)) {
            ++loaded;
        }

        free(full_path);
    } while (FindNextFileA(find_handle, &find_data) != 0);

    FindClose(find_handle);
#else
    DIR *dir = opendir(directory);
    if (!dir) {
        return 0u;
    }

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
#endif

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
