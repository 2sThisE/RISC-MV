#include "host_thread.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

int test_host_time(void)
{
    uint64_t previous = host_monotonic_nanoseconds();
    assert(previous != 0);
    uint64_t minimum_positive_delta = UINT64_MAX;
    for (size_t sample = 0; sample < 64; ++sample) {
        uint64_t now = host_monotonic_nanoseconds();
        assert(now >= previous);
        if (now > previous && now - previous < minimum_positive_delta) {
            minimum_positive_delta = now - previous;
        }
        previous = now;
    }
    assert(minimum_positive_delta < UINT64_C(1000000));

    HostHighResolutionTimer timer;
    assert(host_high_resolution_timer_init(&timer));
    uint64_t start = host_monotonic_nanoseconds();
    assert(host_high_resolution_timer_wait(&timer, UINT64_C(100000)));
    uint64_t end = host_monotonic_nanoseconds();
    host_high_resolution_timer_destroy(&timer);
    assert(end >= start);
    assert(end - start >= UINT64_C(100000));
    return 0;
}
