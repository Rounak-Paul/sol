#include "sol_system_manager.h"

#include "sol_threading.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct SolServiceEntry {
    char *name;
    void *service;
    SolServiceDestroyFn destroy_fn;
    void *destroy_user_data;
} SolServiceEntry;

struct SolSystemManager {
    pthread_mutex_t lock;

    SolEventBus *events;
    SolBufferSystem *buffers;
    SolJobSystem *jobs;
    SolInputSystem *input;
    SolPluginManager *plugins;

    SolServiceEntry *services;
    size_t service_count;
    size_t service_capacity;
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

static bool sol_system_reserve_services(SolSystemManager *manager, size_t min_capacity)
{
    if (manager->service_capacity >= min_capacity) {
        return true;
    }

    size_t new_capacity = manager->service_capacity == 0u ? 16u : manager->service_capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2u;
    }

    SolServiceEntry *new_items = (SolServiceEntry *)realloc(
        manager->services,
        new_capacity * sizeof(SolServiceEntry)
    );

    if (!new_items) {
        return false;
    }

    manager->services = new_items;
    manager->service_capacity = new_capacity;
    return true;
}

static bool sol_system_register_builtin_services(SolSystemManager *manager)
{
    return sol_system_register_service(manager, "core.events", manager->events, NULL, NULL) &&
           sol_system_register_service(manager, "core.buffers", manager->buffers, NULL, NULL) &&
           sol_system_register_service(manager, "core.jobs", manager->jobs, NULL, NULL) &&
           sol_system_register_service(manager, "core.input", manager->input, NULL, NULL) &&
           sol_system_register_service(manager, "core.plugins", manager->plugins, NULL, NULL);
}

static void sol_system_destroy_services(SolSystemManager *manager)
{
    for (size_t i = 0u; i < manager->service_count; ++i) {
        SolServiceEntry *entry = &manager->services[i];
        if (entry->destroy_fn) {
            entry->destroy_fn(entry->service, entry->destroy_user_data);
        }
        free(entry->name);
    }

    free(manager->services);
    manager->services = NULL;
    manager->service_count = 0u;
    manager->service_capacity = 0u;
}

SolSystemConfig sol_system_config_default(void)
{
    SolSystemConfig config;
    config.events = sol_event_bus_config_default();
    config.buffers = sol_buffer_system_config_default();
    config.jobs = sol_job_system_config_default();
    config.input = sol_input_config_default();
    config.plugins = sol_plugin_manager_config_default();
    return config;
}

SolSystemManager *sol_system_manager_create(const SolSystemConfig *config)
{
    SolSystemConfig effective = config ? *config : sol_system_config_default();

    SolSystemManager *manager = (SolSystemManager *)calloc(1u, sizeof(SolSystemManager));
    if (!manager) {
        return NULL;
    }

    if (pthread_mutex_init(&manager->lock, NULL) != 0) {
        free(manager);
        return NULL;
    }

    manager->events = sol_event_bus_create(&effective.events);
    if (!manager->events) {
        sol_system_manager_destroy(manager);
        return NULL;
    }

    manager->buffers = sol_buffer_system_create(&effective.buffers);
    if (!manager->buffers) {
        sol_system_manager_destroy(manager);
        return NULL;
    }

    manager->jobs = sol_job_system_create(&effective.jobs);
    if (!manager->jobs) {
        sol_system_manager_destroy(manager);
        return NULL;
    }

    effective.input.event_bus = manager->events;
    manager->input = sol_input_system_create(&effective.input);
    if (!manager->input) {
        sol_system_manager_destroy(manager);
        return NULL;
    }

    manager->plugins = sol_plugin_manager_create(manager, &effective.plugins);
    if (!manager->plugins) {
        sol_system_manager_destroy(manager);
        return NULL;
    }

    if (!sol_system_register_builtin_services(manager)) {
        sol_system_manager_destroy(manager);
        return NULL;
    }

    return manager;
}

void sol_system_manager_destroy(SolSystemManager *manager)
{
    if (!manager) {
        return;
    }

    if (manager->plugins) {
        sol_plugin_manager_destroy(manager->plugins);
        manager->plugins = NULL;
    }

    sol_system_destroy_services(manager);

    if (manager->input) {
        sol_input_system_destroy(manager->input);
        manager->input = NULL;
    }

    if (manager->jobs) {
        sol_job_system_destroy(manager->jobs);
        manager->jobs = NULL;
    }

    if (manager->buffers) {
        sol_buffer_system_destroy(manager->buffers);
        manager->buffers = NULL;
    }

    if (manager->events) {
        sol_event_bus_destroy(manager->events);
        manager->events = NULL;
    }

    pthread_mutex_destroy(&manager->lock);
    free(manager);
}

SolEventBus *sol_system_events(SolSystemManager *manager)
{
    return manager ? manager->events : NULL;
}

SolBufferSystem *sol_system_buffers(SolSystemManager *manager)
{
    return manager ? manager->buffers : NULL;
}

SolJobSystem *sol_system_jobs(SolSystemManager *manager)
{
    return manager ? manager->jobs : NULL;
}

SolInputSystem *sol_system_input(SolSystemManager *manager)
{
    return manager ? manager->input : NULL;
}

SolPluginManager *sol_system_plugins(SolSystemManager *manager)
{
    return manager ? manager->plugins : NULL;
}

bool sol_system_register_service(
    SolSystemManager *manager,
    const char *name,
    void *service,
    SolServiceDestroyFn destroy_fn,
    void *destroy_user_data
)
{
    if (!manager || !name || *name == '\0') {
        return false;
    }

    char *name_copy = sol_strdup(name);
    if (!name_copy) {
        return false;
    }

    pthread_mutex_lock(&manager->lock);

    for (size_t i = 0u; i < manager->service_count; ++i) {
        if (strcmp(manager->services[i].name, name) == 0) {
            pthread_mutex_unlock(&manager->lock);
            free(name_copy);
            return false;
        }
    }

    if (!sol_system_reserve_services(manager, manager->service_count + 1u)) {
        pthread_mutex_unlock(&manager->lock);
        free(name_copy);
        return false;
    }

    SolServiceEntry entry;
    entry.name = name_copy;
    entry.service = service;
    entry.destroy_fn = destroy_fn;
    entry.destroy_user_data = destroy_user_data;

    manager->services[manager->service_count++] = entry;

    pthread_mutex_unlock(&manager->lock);
    return true;
}

void *sol_system_get_service(SolSystemManager *manager, const char *name)
{
    if (!manager || !name || *name == '\0') {
        return NULL;
    }

    pthread_mutex_lock(&manager->lock);

    void *service = NULL;
    for (size_t i = 0u; i < manager->service_count; ++i) {
        if (strcmp(manager->services[i].name, name) == 0) {
            service = manager->services[i].service;
            break;
        }
    }

    pthread_mutex_unlock(&manager->lock);
    return service;
}

bool sol_system_unregister_service(SolSystemManager *manager, const char *name)
{
    if (!manager || !name || *name == '\0') {
        return false;
    }

    SolServiceEntry removed;
    memset(&removed, 0, sizeof(removed));

    bool found = false;

    pthread_mutex_lock(&manager->lock);
    for (size_t i = 0u; i < manager->service_count; ++i) {
        if (strcmp(manager->services[i].name, name) != 0) {
            continue;
        }

        removed = manager->services[i];

        for (size_t j = i + 1u; j < manager->service_count; ++j) {
            manager->services[j - 1u] = manager->services[j];
        }

        --manager->service_count;
        found = true;
        break;
    }
    pthread_mutex_unlock(&manager->lock);

    if (!found) {
        return false;
    }

    if (removed.destroy_fn) {
        removed.destroy_fn(removed.service, removed.destroy_user_data);
    }

    free(removed.name);
    return true;
}

void sol_system_begin_frame(SolSystemManager *manager)
{
    if (!manager) {
        return;
    }

    sol_input_system_begin_frame(manager->input);
}

size_t sol_system_pump_events(SolSystemManager *manager, size_t max_events)
{
    if (!manager) {
        return 0u;
    }

    return sol_event_bus_drain(manager->events, max_events);
}

void sol_system_end_frame(SolSystemManager *manager)
{
    (void)manager;
}

bool sol_system_load_plugin(SolSystemManager *manager, const char *plugin_path)
{
    if (!manager || !plugin_path) {
        return false;
    }

    return sol_plugin_manager_load(manager->plugins, plugin_path);
}

size_t sol_system_load_plugins_from_directory(SolSystemManager *manager, const char *directory_path)
{
    if (!manager) {
        return 0u;
    }

    return sol_plugin_manager_load_directory(manager->plugins, directory_path);
}
