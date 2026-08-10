#include "bus.h"
#include "interrupt.h"
#include "ram.h"
#include "timer.h"

#include <assert.h>
#include <stdint.h>

#define TIMER_TEST_BASE UINT64_C(0xFFFFFFFFFFFF0000)

int test_bus_timer(void)
{
    uint8_t memory[32] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    /* 일반 RAM의 비정렬 little-endian 접근을 확인한다. */
    assert(bus_write(&bus, 3, 4, UINT64_C(0x12345678)));
    uint64_t value;
    assert(bus_read(&bus, 3, 4, &value));
    assert(value == UINT64_C(0x12345678));

    TimerDevice timer;
    assert(timer_device_init(&timer, TIMER_INTERRUPT_LINE));
    assert(bus_map_device(&bus,
                          TIMER_TEST_BASE,
                          TIMER_MMIO_SIZE,
                          timer_device_as_bus_device(&timer)));

    /* RAM이나 기존 장치와 겹치는 매핑은 거부한다. */
    assert(!bus_map_device(&bus,
                           8,
                           TIMER_MMIO_SIZE,
                           timer_device_as_bus_device(&timer)));
    assert(!bus_map_device(&bus,
                           TIMER_TEST_BASE + 8,
                           TIMER_MMIO_SIZE,
                           timer_device_as_bus_device(&timer)));

    InterruptController interrupts;
    interrupt_controller_init(&interrupts);
    InterruptController *targets[1] = { &interrupts };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 1));

    assert(bus_write(&bus,
                     TIMER_TEST_BASE + TIMER_COMPARE_OFFSET,
                     8,
                     5));
    assert(bus_write(&bus,
                     TIMER_TEST_BASE + TIMER_CONTROL_OFFSET,
                     8,
                     TIMER_CONTROL_ENABLE |
                     TIMER_CONTROL_REPEAT |
                     TIMER_CONTROL_IRQ_ENABLE));

    bus_tick(&bus, 12, &router);
    assert(bus_read(&bus,
                    TIMER_TEST_BASE + TIMER_COUNTER_OFFSET,
                    8,
                    &value));
    assert(value == 2);

    unsigned int line;
    assert(interrupt_controller_take_next(&interrupts, &line));
    assert(line == TIMER_INTERRUPT_LINE);

    /* 남은 3 tick으로 다음 반복 주기에 도달한다. */
    bus_tick(&bus, 3, &router);
    assert(interrupt_controller_take_next(&interrupts, &line));

    assert(bus_write(&bus,
                     TIMER_TEST_BASE + TIMER_STATUS_OFFSET,
                     8,
                     TIMER_STATUS_PENDING));
    assert(bus_read(&bus,
                    TIMER_TEST_BASE + TIMER_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == 0);

    /* 타이머 MMIO는 64비트 접근만 허용한다. */
    assert(!bus_read(&bus,
                     TIMER_TEST_BASE + TIMER_STATUS_OFFSET,
                     4,
                     &value));
    return 0;
}
