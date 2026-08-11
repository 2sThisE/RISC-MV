#include "host_thread.h"

#include <stddef.h>

#if defined(_WIN32)

static DWORD WINAPI host_thread_entry(LPVOID context)
{
    HostThread *thread = context;
    thread->result = thread->function(thread->context);
    return 0;
}

int host_thread_create(HostThread *thread,
                       HostThreadFunction function,
                       void *context)
{
    if (thread == NULL || function == NULL) {
        return 0;
    }

    thread->function = function;
    thread->context = context;
    thread->result = 0;
    thread->handle = CreateThread(NULL,
                                  0,
                                  host_thread_entry,
                                  thread,
                                  0,
                                  NULL);
    return thread->handle != NULL;
}

int host_thread_join(HostThread *thread, int *result)
{
    if (thread == NULL || thread->handle == NULL) {
        return 0;
    }

    if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) {
        return 0;
    }

    if (result != NULL) {
        *result = thread->result;
    }
    CloseHandle(thread->handle);
    thread->handle = NULL;
    return 1;
}

int host_event_init(HostEvent *event)
{
    if (event == NULL) return 0;
    event->handle = CreateEventW(NULL, FALSE, FALSE, NULL);
    return event->handle != NULL;
}

void host_event_destroy(HostEvent *event)
{
    if (event == NULL || event->handle == NULL) return;
    CloseHandle(event->handle);
    event->handle = NULL;
}

int host_event_signal(HostEvent *event)
{
    return event != NULL && event->handle != NULL &&
           SetEvent(event->handle) != 0;
}

int host_event_wait(HostEvent *event)
{
    return event != NULL && event->handle != NULL &&
           WaitForSingleObject(event->handle, INFINITE) == WAIT_OBJECT_0;
}

void host_thread_sleep_milliseconds(uint32_t milliseconds)
{
    Sleep(milliseconds);
}

uint64_t host_monotonic_milliseconds(void)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return GetTickCount64();
    }

    uint64_t seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
    uint64_t remainder =
        (uint64_t)(counter.QuadPart % frequency.QuadPart);
    return seconds * UINT64_C(1000) +
           remainder * UINT64_C(1000) / (uint64_t)frequency.QuadPart;
}

#else

#include <errno.h>
#include <time.h>

static void *host_thread_entry(void *context)
{
    HostThread *thread = context;
    thread->result = thread->function(thread->context);
    return NULL;
}

int host_thread_create(HostThread *thread,
                       HostThreadFunction function,
                       void *context)
{
    if (thread == NULL || function == NULL) {
        return 0;
    }

    thread->function = function;
    thread->context = context;
    thread->result = 0;
    return pthread_create(&thread->handle,
                          NULL,
                          host_thread_entry,
                          thread) == 0;
}

int host_thread_join(HostThread *thread, int *result)
{
    if (thread == NULL || pthread_join(thread->handle, NULL) != 0) {
        return 0;
    }

    if (result != NULL) {
        *result = thread->result;
    }
    return 1;
}

int host_event_init(HostEvent *event)
{
    if (event == NULL || pthread_mutex_init(&event->mutex, NULL) != 0) {
        return 0;
    }
    if (pthread_cond_init(&event->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&event->mutex);
        return 0;
    }
    event->signaled = 0;
    return 1;
}

void host_event_destroy(HostEvent *event)
{
    if (event == NULL) return;
    (void)pthread_cond_destroy(&event->condition);
    (void)pthread_mutex_destroy(&event->mutex);
}

int host_event_signal(HostEvent *event)
{
    if (event == NULL || pthread_mutex_lock(&event->mutex) != 0) return 0;
    event->signaled = 1;
    int result = pthread_cond_signal(&event->condition) == 0;
    (void)pthread_mutex_unlock(&event->mutex);
    return result;
}

int host_event_wait(HostEvent *event)
{
    if (event == NULL || pthread_mutex_lock(&event->mutex) != 0) return 0;
    int result = 1;
    while (!event->signaled) {
        if (pthread_cond_wait(&event->condition, &event->mutex) != 0) {
            result = 0;
            break;
        }
    }
    if (result) event->signaled = 0;
    (void)pthread_mutex_unlock(&event->mutex);
    return result;
}

void host_thread_sleep_milliseconds(uint32_t milliseconds)
{
    struct timespec duration = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };

    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

uint64_t host_monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

#endif
