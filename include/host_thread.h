#ifndef HOST_THREAD_H
#define HOST_THREAD_H

#include <stdint.h>

typedef int (*HostThreadFunction)(void *context);

#if defined(_WIN32)
#include <windows.h>

typedef struct {
    HANDLE handle;
    HostThreadFunction function;
    void *context;
    int result;
} HostThread;
#else
#include <pthread.h>

typedef struct {
    pthread_t handle;
    HostThreadFunction function;
    void *context;
    int result;
} HostThread;
#endif

int host_thread_create(HostThread *thread,
                       HostThreadFunction function,
                       void *context);
int host_thread_join(HostThread *thread, int *result);
void host_thread_sleep_milliseconds(uint32_t milliseconds);
uint64_t host_monotonic_milliseconds(void);

#endif
