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
