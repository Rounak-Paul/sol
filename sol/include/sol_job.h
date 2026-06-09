#ifndef SOL_JOB_H
#define SOL_JOB_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SolJobSystem SolJobSystem;
typedef struct SolJobFence SolJobFence;

/* A single unit of work submitted to the job system. */
typedef void (*SolJobFn)(void *user_data);

/*
 * Worker callback for parallel_for, invoked once per chunk.
 *
 * begin      Inclusive start index of this chunk's item range.
 * end        Exclusive end index of this chunk's item range.
 * user_data  Caller-supplied context shared across all chunks.
 */
typedef void (*SolParallelForRangeFn)(uint32_t begin, uint32_t end, void *user_data);

/* Tuning knobs for the job system's thread pool and work queue. */
typedef struct SolJobSystemConfig {
    uint32_t worker_count;
    uint32_t queue_capacity;
} SolJobSystemConfig;

/* Return a SolJobSystemConfig populated with sensible defaults. */
SolJobSystemConfig sol_job_system_config_default(void);

/*
 * Create a new job system and start its worker threads.
 *
 * config  Thread count and queue capacity; use sol_job_system_config_default().
 * Returns A heap-allocated system, or NULL on failure.
 */
SolJobSystem *sol_job_system_create(const SolJobSystemConfig *config);

/* Drain remaining jobs, stop worker threads, and free the system. */
void sol_job_system_destroy(SolJobSystem *system);

/*
 * Enqueue a single job for asynchronous execution.
 *
 * system     The job system.
 * fn         Function to execute on a worker thread.
 * user_data  Passed unchanged to fn.
 * fence      Optional fence to track completion; pass NULL to ignore.
 * Returns    true if the job was enqueued successfully.
 */
bool sol_job_system_submit(
    SolJobSystem *system,
    SolJobFn fn,
    void *user_data,
    SolJobFence *fence
);

/*
 * Split a range into chunks and enqueue each chunk as a separate job.
 *
 * system      The job system.
 * item_count  Total number of items to process.
 * chunk_size  Number of items per chunk.
 * fn          Worker function called once per chunk with [begin, end).
 * user_data   Passed unchanged to every fn invocation.
 * Returns     true if all chunks were enqueued successfully.
 */
bool sol_job_system_parallel_for(
    SolJobSystem *system,
    uint32_t item_count,
    uint32_t chunk_size,
    SolParallelForRangeFn fn,
    void *user_data
);

/* Block the calling thread until all enqueued jobs have completed. */
void sol_job_system_wait_idle(SolJobSystem *system);

/* Returns the number of worker threads in the pool. */
uint32_t sol_job_system_worker_count(const SolJobSystem *system);

/* Create a new fence with an initial pending count of zero. */
SolJobFence *sol_job_fence_create(void);

/* Destroy a fence. Must not be called while jobs are still pending on it. */
void sol_job_fence_destroy(SolJobFence *fence);

/* Block the calling thread until all jobs associated with this fence complete. */
void sol_job_fence_wait(SolJobFence *fence);

/* Returns the number of jobs still pending on this fence. */
uint32_t sol_job_fence_pending(SolJobFence *fence);

#endif
