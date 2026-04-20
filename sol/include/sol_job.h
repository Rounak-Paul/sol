#ifndef SOL_JOB_H
#define SOL_JOB_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SolJobSystem SolJobSystem;
typedef struct SolJobFence SolJobFence;

typedef void (*SolJobFn)(void *user_data);
typedef void (*SolParallelForRangeFn)(uint32_t begin, uint32_t end, void *user_data);

typedef struct SolJobSystemConfig {
    uint32_t worker_count;
    uint32_t queue_capacity;
} SolJobSystemConfig;

SolJobSystemConfig sol_job_system_config_default(void);

SolJobSystem *sol_job_system_create(const SolJobSystemConfig *config);
void sol_job_system_destroy(SolJobSystem *system);

bool sol_job_system_submit(
    SolJobSystem *system,
    SolJobFn fn,
    void *user_data,
    SolJobFence *fence
);

bool sol_job_system_parallel_for(
    SolJobSystem *system,
    uint32_t item_count,
    uint32_t chunk_size,
    SolParallelForRangeFn fn,
    void *user_data
);

void sol_job_system_wait_idle(SolJobSystem *system);
uint32_t sol_job_system_worker_count(const SolJobSystem *system);

SolJobFence *sol_job_fence_create(void);
void sol_job_fence_destroy(SolJobFence *fence);

void sol_job_fence_wait(SolJobFence *fence);
uint32_t sol_job_fence_pending(SolJobFence *fence);

#endif
