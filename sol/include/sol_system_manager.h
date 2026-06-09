#ifndef SOL_SYSTEM_MANAGER_H
#define SOL_SYSTEM_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "sol_event.h"
#include "sol_buffer.h"
#include "sol_input.h"
#include "sol_job.h"
#include "sol_plugin.h"

typedef struct SolSystemManager SolSystemManager;

/* Destructor callback invoked when a registered service is unregistered or the manager is destroyed. */
typedef void (*SolServiceDestroyFn)(void *service, void *user_data);

/* Aggregate configuration for all subsystems managed by SolSystemManager. */
typedef struct SolSystemConfig {
    SolEventBusConfig events;
    SolBufferSystemConfig buffers;
    SolJobSystemConfig jobs;
    SolInputConfig input;
    SolPluginManagerConfig plugins;
} SolSystemConfig;

/* Return a SolSystemConfig populated with sensible defaults for all subsystems. */
SolSystemConfig sol_system_config_default(void);

/*
 * Create a system manager and initialise all owned subsystems.
 *
 * config  Per-subsystem configuration; use sol_system_config_default().
 * Returns A heap-allocated manager, or NULL on failure.
 */
SolSystemManager *sol_system_manager_create(const SolSystemConfig *config);

/* Destroy the system manager and all owned subsystems in safe teardown order. */
void sol_system_manager_destroy(SolSystemManager *manager);

/* Returns the event bus owned by this manager. */
SolEventBus *sol_system_events(SolSystemManager *manager);

/* Returns the buffer system owned by this manager. */
SolBufferSystem *sol_system_buffers(SolSystemManager *manager);

/* Returns the job system owned by this manager. */
SolJobSystem *sol_system_jobs(SolSystemManager *manager);

/* Returns the input system owned by this manager. */
SolInputSystem *sol_system_input(SolSystemManager *manager);

/* Returns the plugin manager owned by this manager. */
SolPluginManager *sol_system_plugins(SolSystemManager *manager);

/*
 * Register a named service (version 0) in the manager's service registry.
 *
 * manager          The system manager.
 * name             Unique service name.
 * service          Pointer to the service object.
 * destroy_fn       Optional destructor; may be NULL.
 * destroy_user_data  Passed unchanged to destroy_fn.
 * Returns          true on success.
 */
bool sol_system_register_service(
    SolSystemManager *manager,
    const char *name,
    void *service,
    SolServiceDestroyFn destroy_fn,
    void *destroy_user_data
);

/*
 * Register a named service with an explicit version number.
 *
 * sol_system_register_service is a wrapper around this with version=0.
 *
 * manager            The system manager.
 * name               Unique service name.
 * version            Service ABI version number.
 * service            Pointer to the service object.
 * destroy_fn         Optional destructor; may be NULL.
 * destroy_user_data  Passed unchanged to destroy_fn.
 * Returns            true on success.
 */
bool sol_system_register_service_v(
    SolSystemManager   *manager,
    const char         *name,
    uint32_t            version,
    void               *service,
    SolServiceDestroyFn destroy_fn,
    void               *destroy_user_data
);

/*
 * Retrieve a registered service by name (any version).
 *
 * manager  The system manager.
 * name     Service name to look up.
 * Returns  The service pointer, or NULL when not found.
 */
void *sol_system_get_service(SolSystemManager *manager, const char *name);

/*
 * Retrieve a registered service, enforcing a minimum version.
 *
 * manager      The system manager.
 * name         Service name to look up.
 * min_version  Minimum acceptable version.
 * Returns      The service pointer, or NULL if not found or version too low.
 */
void *sol_system_get_service_v(SolSystemManager *manager,
                                const char       *name,
                                uint32_t          min_version);

/*
 * Remove a named service from the registry and invoke its destructor.
 *
 * manager  The system manager.
 * name     Service name to unregister.
 * Returns  true if the service was found and removed.
 */
bool sol_system_unregister_service(SolSystemManager *manager, const char *name);

/* Begin a frame: resets per-frame input state and similar transient data. */
void sol_system_begin_frame(SolSystemManager *manager);

/*
 * Drain queued events from the event bus.
 *
 * manager     The system manager.
 * max_events  Maximum number of events to deliver this call.
 * Returns     Number of events actually dispatched.
 */
size_t sol_system_pump_events(SolSystemManager *manager, size_t max_events);

/* End a frame: performs any post-frame cleanup. */
void sol_system_end_frame(SolSystemManager *manager);

/*
 * Load a single plugin from a shared-library path.
 *
 * manager      The system manager.
 * plugin_path  Path to the .dylib / .so / .dll file.
 * Returns      true if the plugin loaded successfully.
 */
bool sol_system_load_plugin(SolSystemManager *manager, const char *plugin_path);

/*
 * Load all plugins from a directory in dependency order.
 *
 * manager         The system manager.
 * directory_path  Directory to scan, or NULL for the configured default.
 * Returns         Number of plugins successfully loaded.
 */
size_t sol_system_load_plugins_from_directory(SolSystemManager *manager, const char *directory_path);

#endif
