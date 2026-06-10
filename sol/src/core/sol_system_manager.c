#include "sol_system_manager.h"

#include "sol_threading.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct SolServiceEntry {
    char *name;
    uint32_t version;        /* 0 when registered via the non-versioned API */
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

/*
 * Duplicate a string (allocate and copy).
 *
 * value  String to duplicate.
 * Returns Newly allocated copy or NULL if value is NULL.
 */
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

/*
 * Ensure the service array has capacity for at least min_capacity entries.
 *
 * manager       System manager.
 * min_capacity  Minimum capacity needed.
 * Returns True if capacity is available or was allocated successfully.
 */
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

/*
 * Register all built-in system services (events, buffers, jobs, input, plugins).
 *
 * manager  System manager.
 * Returns True if all services were registered successfully.
 */
static bool sol_system_register_builtin_services(SolSystemManager *manager)
{
    return sol_system_register_service(manager, "core.events", manager->events, NULL, NULL) &&
           sol_system_register_service(manager, "core.buffers", manager->buffers, NULL, NULL) &&
           sol_system_register_service(manager, "core.jobs", manager->jobs, NULL, NULL) &&
           sol_system_register_service(manager, "core.input", manager->input, NULL, NULL) &&
           sol_system_register_service(manager, "core.plugins", manager->plugins, NULL, NULL);
}

/*
 * Unregister and deallocate all registered services.
 *
 * manager  System manager whose services will be destroyed.
 */
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

/*
 * Create a default system configuration with all subsystems enabled.
 *
 * Returns Default configuration struct.
 */
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

/*
 * Create and initialize the system manager with all subsystems.
 *
 * config  Optional configuration (uses default if NULL).
 * Returns Newly allocated system manager or NULL on failure.
 */
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

/*
 * Deallocate the system manager and all subsystems.
 *
 * manager  System manager to destroy (NULL-safe).
 */
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

/*
 * Get the event bus subsystem.
 *
 * manager  System manager.
 * Returns Event bus or NULL.
 */
SolEventBus *sol_system_events(SolSystemManager *manager)
{
    return manager ? manager->events : NULL;
}

/*
 * Get the buffer system subsystem.
 *
 * manager  System manager.
 * Returns Buffer system or NULL.
 */
SolBufferSystem *sol_system_buffers(SolSystemManager *manager)
{
    return manager ? manager->buffers : NULL;
}

/*
 * Get the job system subsystem.
 *
 * manager  System manager.
 * Returns Job system or NULL.
 */
SolJobSystem *sol_system_jobs(SolSystemManager *manager)
{
    return manager ? manager->jobs : NULL;
}

/*
 * Get the input system subsystem.
 *
 * manager  System manager.
 * Returns Input system or NULL.
 */
SolInputSystem *sol_system_input(SolSystemManager *manager)
{
    return manager ? manager->input : NULL;
}

/*
 * Get the plugin manager subsystem.
 *
 * manager  System manager.
 * Returns Plugin manager or NULL.
 */
SolPluginManager *sol_system_plugins(SolSystemManager *manager)
{
    return manager ? manager->plugins : NULL;
}

/*
 * Register a service with the system manager (non-versioned).
 *
 * manager            System manager.
 * name               Unique service name.
 * service            Service pointer (opaque).
 * destroy_fn         Optional destructor function.
 * destroy_user_data  User data passed to the destructor.
 * Returns True if registration succeeded.
 */
bool sol_system_register_service(
    SolSystemManager *manager,
    const char *name,
    void *service,
    SolServiceDestroyFn destroy_fn,
    void *destroy_user_data
)
{
    return sol_system_register_service_v(manager, name, 0u,
                                          service, destroy_fn, destroy_user_data);
}

/*
 * Register a versioned service with the system manager.
 *
 * manager            System manager.
 * name               Unique service name.
 * version            Service version (0 means unversioned).
 * service            Service pointer (opaque).
 * destroy_fn         Optional destructor function.
 * destroy_user_data  User data passed to the destructor.
 * Returns True if registration succeeded.
 */
bool sol_system_register_service_v(
    SolSystemManager   *manager,
    const char         *name,
    uint32_t            version,
    void               *service,
    SolServiceDestroyFn destroy_fn,
    void               *destroy_user_data
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
    entry.name             = name_copy;
    entry.version          = version;
    entry.service          = service;
    entry.destroy_fn       = destroy_fn;
    entry.destroy_user_data = destroy_user_data;

    manager->services[manager->service_count++] = entry;

    pthread_mutex_unlock(&manager->lock);
    return true;
}

/*
 * Get a service by name (non-versioned lookup).
 *
 * manager  System manager.
 * name     Service name.
 * Returns Service pointer or NULL if not found.
 */
void *sol_system_get_service(SolSystemManager *manager, const char *name)
{
    return sol_system_get_service_v(manager, name, 0u);
}

/*
 * Get a versioned service by name with minimum version check.
 *
 * manager      System manager.
 * name         Service name.
 * min_version  Minimum acceptable version (0 for any version).
 * Returns Service pointer or NULL if not found or version too low.
 */
void *sol_system_get_service_v(SolSystemManager *manager,
                                const char       *name,
                                uint32_t          min_version)
{
    if (!manager || !name || *name == '\0') {
        return NULL;
    }

    pthread_mutex_lock(&manager->lock);

    void *service = NULL;
    for (size_t i = 0u; i < manager->service_count; ++i) {
        if (strcmp(manager->services[i].name, name) == 0) {
            if (manager->services[i].version >= min_version)
                service = manager->services[i].service;
            break;
        }
    }

    pthread_mutex_unlock(&manager->lock);
    return service;
}

/*
 * Unregister and destroy a service by name.
 *
 * manager  System manager.
 * name     Service name.
 * Returns True if the service was found and unregistered.
 */
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

/*
 * Mark the start of a frame, reset per-frame state in input system.
 *
 * manager  System manager.
 */
void sol_system_begin_frame(SolSystemManager *manager)
{
    if (!manager) {
        return;
    }

    sol_input_system_begin_frame(manager->input);
}

/*
 * Process and drain pending events from the event bus.
 *
 * manager     System manager.
 * max_events  Maximum number of events to process.
 * Returns Number of events processed.
 */
size_t sol_system_pump_events(SolSystemManager *manager, size_t max_events)
{
    if (!manager) {
        return 0u;
    }

    return sol_event_bus_drain(manager->events, max_events);
}

/*
 * Mark the end of a frame, finalize per-frame updates.
 *
 * manager  System manager.
 */
void sol_system_end_frame(SolSystemManager *manager)
{
    (void)manager;
}

/*
 * Load a plugin from the specified file path.
 *
 * manager      System manager.
 * plugin_path  Path to the plugin file.
 * Returns True if the plugin was loaded successfully.
 */
bool sol_system_load_plugin(SolSystemManager *manager, const char *plugin_path)
{
    if (!manager || !plugin_path) {
        return false;
    }

    return sol_plugin_manager_load(manager->plugins, plugin_path);
}

/*
 * Load all plugins from a directory.
 *
 * manager          System manager.
 * directory_path   Path to directory containing plugins.
 * Returns Number of plugins successfully loaded.
 */
size_t sol_system_load_plugins_from_directory(SolSystemManager *manager, const char *directory_path)
{
    if (!manager) {
        return 0u;
    }

    return sol_plugin_manager_load_directory(manager->plugins, directory_path);
}
