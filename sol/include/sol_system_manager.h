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

typedef void (*SolServiceDestroyFn)(void *service, void *user_data);

typedef struct SolSystemConfig {
    SolEventBusConfig events;
    SolBufferSystemConfig buffers;
    SolJobSystemConfig jobs;
    SolInputConfig input;
    SolPluginManagerConfig plugins;
} SolSystemConfig;

SolSystemConfig sol_system_config_default(void);

SolSystemManager *sol_system_manager_create(const SolSystemConfig *config);
void sol_system_manager_destroy(SolSystemManager *manager);

SolEventBus *sol_system_events(SolSystemManager *manager);
SolBufferSystem *sol_system_buffers(SolSystemManager *manager);
SolJobSystem *sol_system_jobs(SolSystemManager *manager);
SolInputSystem *sol_system_input(SolSystemManager *manager);
SolPluginManager *sol_system_plugins(SolSystemManager *manager);

bool sol_system_register_service(
    SolSystemManager *manager,
    const char *name,
    void *service,
    SolServiceDestroyFn destroy_fn,
    void *destroy_user_data
);

/* Versioned variant — `version` is stored alongside the service.
 * Use sol_system_get_service_v with a min_version to get it back;
 * returns NULL when the registered version is below min_version.
 * sol_system_register_service is a wrapper with version=0.           */
bool sol_system_register_service_v(
    SolSystemManager   *manager,
    const char         *name,
    uint32_t            version,
    void               *service,
    SolServiceDestroyFn destroy_fn,
    void               *destroy_user_data
);

void *sol_system_get_service(SolSystemManager *manager, const char *name);

/* Returns the service only when its registered version >= min_version. */
void *sol_system_get_service_v(SolSystemManager *manager,
                                const char       *name,
                                uint32_t          min_version);

bool sol_system_unregister_service(SolSystemManager *manager, const char *name);

void sol_system_begin_frame(SolSystemManager *manager);
size_t sol_system_pump_events(SolSystemManager *manager, size_t max_events);
void sol_system_end_frame(SolSystemManager *manager);

bool sol_system_load_plugin(SolSystemManager *manager, const char *plugin_path);
size_t sol_system_load_plugins_from_directory(SolSystemManager *manager, const char *directory_path);

#endif
