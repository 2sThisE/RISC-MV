#include "bus.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>

int test_atomic(void)
{
    uint8_t memory[64] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));
    assert(ram_write(&ram, 8, 8, 10));

    uint64_t expected = 10;
    assert(bus_compare_exchange64(&bus, 8, &expected, 20));
    assert(expected == 10);

    uint64_t value;
    assert(ram_read(&ram, 8, 8, &value));
    assert(value == 20);

    expected = 15;
    assert(bus_compare_exchange64(&bus, 8, &expected, 30));
    assert(expected == 20);
    assert(ram_read(&ram, 8, 8, &value));
    assert(value == 20);

    value = 99;
    assert(bus_exchange64(&bus, 8, &value));
    assert(value == 20);

    value = 3;
    assert(bus_fetch_add64(&bus, 8, &value));
    assert(value == 99);
    assert(ram_read(&ram, 8, 8, &value));
    assert(value == 102);

    /* 64비트 atomic 주소는 자연 정렬되어야 한다. */
    expected = 0;
    assert(!bus_compare_exchange64(&bus, 9, &expected, 1));
    return 0;
}
