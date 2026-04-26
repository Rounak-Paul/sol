#ifndef SOL_THREADING_H
#define SOL_THREADING_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <process.h>
#include <stdlib.h>
#include <windows.h>

typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

typedef struct SolPthreadStartContext {
    void *(*routine)(void *);
    void *arg;
} SolPthreadStartContext;

static unsigned __stdcall sol_pthread_start(void *context)
{
    SolPthreadStartContext *start = (SolPthreadStartContext *)context;
    if (start && start->routine) {
        start->routine(start->arg);
    }
    free(start);
    return 0u;
}

static int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void)attr;
    InitializeCriticalSection(mutex);
    return 0;
}

static int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    DeleteCriticalSection(mutex);
    return 0;
}

static int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    EnterCriticalSection(mutex);
    return 0;
}

static int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    LeaveCriticalSection(mutex);
    return 0;
}

static int pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    (void)attr;
    InitializeConditionVariable(cond);
    return 0;
}

static int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

static int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return SleepConditionVariableCS(cond, mutex, INFINITE) ? 0 : -1;
}

static int pthread_cond_signal(pthread_cond_t *cond)
{
    WakeConditionVariable(cond);
    return 0;
}

static int pthread_cond_broadcast(pthread_cond_t *cond)
{
    WakeAllConditionVariable(cond);
    return 0;
}

static int pthread_create(
    pthread_t *thread,
    const void *attr,
    void *(*start_routine)(void *),
    void *arg
)
{
    (void)attr;

    SolPthreadStartContext *start = (SolPthreadStartContext *)malloc(sizeof(SolPthreadStartContext));
    if (!start) {
        return -1;
    }

    start->routine = start_routine;
    start->arg = arg;

    uintptr_t handle = _beginthreadex(NULL, 0u, sol_pthread_start, start, 0u, NULL);
    if (handle == 0u) {
        free(start);
        return -1;
    }

    *thread = (HANDLE)handle;
    return 0;
}

static int pthread_join(pthread_t thread, void **retval)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (retval) {
        *retval = NULL;
    }
    return 0;
}

#else

#include <pthread.h>

#endif

#endif