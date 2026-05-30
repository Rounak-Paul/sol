#include "sol_job.h"

#include "sol_platform.h"
#include "sol_threading.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct SolJob {
    SolJobFn fn;
    void *user_data;
    SolJobFence *fence;
} SolJob;

struct SolJobFence {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint32_t pending;
};

struct SolJobSystem {
    pthread_mutex_t lock;
    pthread_cond_t has_work;
    pthread_cond_t idle;

    SolJob *queue;
    uint32_t queue_capacity;
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t queue_count;

    pthread_t *workers;
    uint32_t worker_count;

    uint32_t active_workers;
    bool shutting_down;
};

static bool sol_job_fence_init(SolJobFence *fence)
{
    if (!fence) {
        return false;
    }

    if (pthread_mutex_init(&fence->lock, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&fence->ready, NULL) != 0) {
        pthread_mutex_destroy(&fence->lock);
        return false;
    }

    fence->pending = 0u;
    return true;
}

static void sol_job_fence_deinit(SolJobFence *fence)
{
    if (!fence) {
        return;
    }

    pthread_cond_destroy(&fence->ready);
    pthread_mutex_destroy(&fence->lock);
}

static void sol_job_fence_increment(SolJobFence *fence)
{
    if (!fence) {
        return;
    }

    pthread_mutex_lock(&fence->lock);
    ++fence->pending;
    pthread_mutex_unlock(&fence->lock);
}

static void sol_job_fence_complete(SolJobFence *fence)
{
    if (!fence) {
        return;
    }

    pthread_mutex_lock(&fence->lock);
    if (fence->pending > 0u) {
        --fence->pending;
    }

    if (fence->pending == 0u) {
        pthread_cond_broadcast(&fence->ready);
    }
    pthread_mutex_unlock(&fence->lock);
}

static void *sol_job_worker_main(void *arg)
{
    SolJobSystem *system = (SolJobSystem *)arg;

    for (;;) {
        SolJob job;
        memset(&job, 0, sizeof(job));

        pthread_mutex_lock(&system->lock);
        while (!system->shutting_down && system->queue_count == 0u) {
            pthread_cond_wait(&system->has_work, &system->lock);
        }

        if (system->shutting_down && system->queue_count == 0u) {
            pthread_mutex_unlock(&system->lock);
            break;
        }

        job = system->queue[system->queue_head];
        system->queue_head = (system->queue_head + 1u) % system->queue_capacity;
        --system->queue_count;
        ++system->active_workers;
        pthread_mutex_unlock(&system->lock);

        if (job.fn) {
            job.fn(job.user_data);
        }

        sol_job_fence_complete(job.fence);

        pthread_mutex_lock(&system->lock);
        if (system->active_workers > 0u) {
            --system->active_workers;
        }

        if (system->queue_count == 0u && system->active_workers == 0u) {
            pthread_cond_broadcast(&system->idle);
        }
        pthread_mutex_unlock(&system->lock);
    }

    return NULL;
}

static uint32_t sol_default_worker_count(void)
{
    const uint32_t cpu_count = sol_platform_cpu_count();
    if (cpu_count <= 1u) {
        return 1u;
    }
    return cpu_count - 1u;
}

SolJobSystemConfig sol_job_system_config_default(void)
{
    SolJobSystemConfig config;
    config.worker_count = sol_default_worker_count();
    config.queue_capacity = 1024u;
    return config;
}

SolJobSystem *sol_job_system_create(const SolJobSystemConfig *config)
{
    SolJobSystemConfig effective = config ? *config : sol_job_system_config_default();
    if (effective.worker_count == 0u) {
        effective.worker_count = sol_default_worker_count();
    }
    if (effective.queue_capacity == 0u) {
        effective.queue_capacity = 1024u;
    }

    SolJobSystem *system = (SolJobSystem *)calloc(1u, sizeof(SolJobSystem));
    if (!system) {
        return NULL;
    }

    if (pthread_mutex_init(&system->lock, NULL) != 0) {
        free(system);
        return NULL;
    }

    if (pthread_cond_init(&system->has_work, NULL) != 0) {
        pthread_mutex_destroy(&system->lock);
        free(system);
        return NULL;
    }

    if (pthread_cond_init(&system->idle, NULL) != 0) {
        pthread_cond_destroy(&system->has_work);
        pthread_mutex_destroy(&system->lock);
        free(system);
        return NULL;
    }

    system->queue_capacity = effective.queue_capacity;
    system->worker_count = effective.worker_count;
    system->queue = (SolJob *)calloc(system->queue_capacity, sizeof(SolJob));
    system->workers = (pthread_t *)calloc(system->worker_count, sizeof(pthread_t));

    if (!system->queue || !system->workers) {
        free(system->queue);
        free(system->workers);
        pthread_cond_destroy(&system->idle);
        pthread_cond_destroy(&system->has_work);
        pthread_mutex_destroy(&system->lock);
        free(system);
        return NULL;
    }

    for (uint32_t i = 0u; i < system->worker_count; ++i) {
        if (pthread_create(&system->workers[i], NULL, sol_job_worker_main, system) != 0) {
            pthread_mutex_lock(&system->lock);
            system->shutting_down = true;
            pthread_cond_broadcast(&system->has_work);
            pthread_mutex_unlock(&system->lock);

            for (uint32_t j = 0u; j < i; ++j) {
                pthread_join(system->workers[j], NULL);
            }

            free(system->queue);
            free(system->workers);
            pthread_cond_destroy(&system->idle);
            pthread_cond_destroy(&system->has_work);
            pthread_mutex_destroy(&system->lock);
            free(system);
            return NULL;
        }
    }

    return system;
}

void sol_job_system_destroy(SolJobSystem *system)
{
    if (!system) {
        return;
    }

    pthread_mutex_lock(&system->lock);
    system->shutting_down = true;
    pthread_cond_broadcast(&system->has_work);
    pthread_mutex_unlock(&system->lock);

    for (uint32_t i = 0u; i < system->worker_count; ++i) {
        pthread_join(system->workers[i], NULL);
    }

    free(system->queue);
    free(system->workers);
    pthread_cond_destroy(&system->idle);
    pthread_cond_destroy(&system->has_work);
    pthread_mutex_destroy(&system->lock);
    free(system);
}

bool sol_job_system_submit(
    SolJobSystem *system,
    SolJobFn fn,
    void *user_data,
    SolJobFence *fence
)
{
    if (!system || !fn) {
        return false;
    }

    sol_job_fence_increment(fence);

    bool submitted = false;

    pthread_mutex_lock(&system->lock);
    if (!system->shutting_down && system->queue_count < system->queue_capacity) {
        SolJob job;
        job.fn = fn;
        job.user_data = user_data;
        job.fence = fence;

        system->queue[system->queue_tail] = job;
        system->queue_tail = (system->queue_tail + 1u) % system->queue_capacity;
        ++system->queue_count;
        submitted = true;

        pthread_cond_signal(&system->has_work);
    }
    pthread_mutex_unlock(&system->lock);

    if (!submitted) {
        sol_job_fence_complete(fence);
    }

    return submitted;
}

typedef struct SolParallelForContext {
    _Atomic uint32_t next_index;
    uint32_t item_count;
    uint32_t chunk_size;
    SolParallelForRangeFn fn;
    void *user_data;
} SolParallelForContext;

static void sol_parallel_for_task(void *user_data)
{
    SolParallelForContext *context = (SolParallelForContext *)user_data;

    for (;;) {
        uint32_t begin = atomic_fetch_add_explicit(
            &context->next_index,
            context->chunk_size,
            memory_order_relaxed
        );

        if (begin >= context->item_count) {
            break;
        }

        uint32_t end = begin + context->chunk_size;
        if (end > context->item_count) {
            end = context->item_count;
        }

        context->fn(begin, end, context->user_data);
    }
}

bool sol_job_system_parallel_for(
    SolJobSystem *system,
    uint32_t item_count,
    uint32_t chunk_size,
    SolParallelForRangeFn fn,
    void *user_data
)
{
    if (!system || !fn) {
        return false;
    }

    if (item_count == 0u) {
        return true;
    }

    if (chunk_size == 0u) {
        chunk_size = 64u;
    }

    SolParallelForContext context;
    atomic_init(&context.next_index, 0u);
    context.item_count = item_count;
    context.chunk_size = chunk_size;
    context.fn = fn;
    context.user_data = user_data;

    const uint32_t chunk_count = (item_count + chunk_size - 1u) / chunk_size;
    const uint32_t launch_count = chunk_count < system->worker_count ? chunk_count : system->worker_count;

    if (launch_count == 0u) {
        fn(0u, item_count, user_data);
        return true;
    }

    SolJobFence *fence = sol_job_fence_create();
    if (!fence) {
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0u; i < launch_count; ++i) {
        if (!sol_job_system_submit(system, sol_parallel_for_task, &context, fence)) {
            ok = false;
            break;
        }
    }

    sol_job_fence_wait(fence);

    sol_job_fence_destroy(fence);
    return ok;
}

void sol_job_system_wait_idle(SolJobSystem *system)
{
    if (!system) {
        return;
    }

    pthread_mutex_lock(&system->lock);
    while (system->queue_count > 0u || system->active_workers > 0u) {
        pthread_cond_wait(&system->idle, &system->lock);
    }
    pthread_mutex_unlock(&system->lock);
}

uint32_t sol_job_system_worker_count(const SolJobSystem *system)
{
    if (!system) {
        return 0u;
    }
    return system->worker_count;
}

SolJobFence *sol_job_fence_create(void)
{
    SolJobFence *fence = (SolJobFence *)calloc(1u, sizeof(SolJobFence));
    if (!fence) {
        return NULL;
    }

    if (!sol_job_fence_init(fence)) {
        free(fence);
        return NULL;
    }

    return fence;
}

void sol_job_fence_destroy(SolJobFence *fence)
{
    if (!fence) {
        return;
    }

    sol_job_fence_wait(fence);
    sol_job_fence_deinit(fence);
    free(fence);
}

void sol_job_fence_wait(SolJobFence *fence)
{
    if (!fence) {
        return;
    }

    pthread_mutex_lock(&fence->lock);
    while (fence->pending > 0u) {
        pthread_cond_wait(&fence->ready, &fence->lock);
    }
    pthread_mutex_unlock(&fence->lock);
}

uint32_t sol_job_fence_pending(SolJobFence *fence)
{
    if (!fence) {
        return 0u;
    }

    pthread_mutex_lock(&fence->lock);
    const uint32_t pending = fence->pending;
    pthread_mutex_unlock(&fence->lock);
    return pending;
}
