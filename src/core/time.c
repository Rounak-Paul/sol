/*
 * Sol IDE - Time Utilities Implementation
 */

#include "sol.h"

#ifdef SOL_PLATFORM_WINDOWS
    #include <windows.h>
#else
    #include <sys/time.h>
    #include <time.h>
#endif

uint64_t sol_time_ms(void) {
#ifdef SOL_PLATFORM_WINDOWS
    return GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

uint64_t sol_time_us(void) {
#ifdef SOL_PLATFORM_WINDOWS
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000 / freq.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
#endif
}
