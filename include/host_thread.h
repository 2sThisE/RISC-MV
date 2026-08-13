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

typedef struct {
    HANDLE handle;
} HostEvent;

typedef struct {
    HANDLE handle;
} HostHighResolutionTimer;
#else
#include <pthread.h>

typedef struct {
    pthread_t handle;
    HostThreadFunction function;
    void *context;
    int result;
} HostThread;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int signaled;
} HostEvent;

typedef struct {
    int initialized;
} HostHighResolutionTimer;
#endif

int host_thread_create(HostThread *thread,
                       HostThreadFunction function,
                       void *context);
int host_thread_join(HostThread *thread, int *result);
int host_event_init(HostEvent *event);
void host_event_destroy(HostEvent *event);
int host_event_signal(HostEvent *event);
int host_event_wait(HostEvent *event);
int host_high_resolution_timer_init(HostHighResolutionTimer *timer);
void host_high_resolution_timer_destroy(HostHighResolutionTimer *timer);
int host_high_resolution_timer_wait(HostHighResolutionTimer *timer,
                                    uint64_t nanoseconds);
void host_thread_sleep_milliseconds(uint32_t milliseconds);
uint64_t host_monotonic_nanoseconds(void);
uint64_t host_monotonic_milliseconds(void);

#endif
