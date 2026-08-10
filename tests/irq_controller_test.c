#include "bus.h"
#include "interrupt.h"
#include "irq_controller.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>

static uint64_t read_register(Bus *bus, uint64_t offset)
{
    uint64_t value = UINT64_MAX;
    assert(bus_read(bus,
                    IRQ_CONTROLLER_MMIO_BASE + offset,
                    sizeof(value),
                    &value));
    return value;
}

static void write_register(Bus *bus, uint64_t offset, uint64_t value)
{
    assert(bus_write(bus,
                     IRQ_CONTROLLER_MMIO_BASE + offset,
                     sizeof(value),
                     value));
}

int test_irq_controller(void)
{
    uint8_t memory[256] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    InterruptController target0;
    InterruptController target1;
    interrupt_controller_init(&target0);
    interrupt_controller_init(&target1);
    InterruptController *targets[] = { &target0, &target1 };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 2));

    IrqControllerDevice device;
    assert(irq_controller_device_init(&device, &router));
    assert(bus_map_device(&bus,
                          IRQ_CONTROLLER_MMIO_BASE,
                          IRQ_CONTROLLER_MMIO_SIZE,
                          irq_controller_device_as_bus_device(&device)));

    assert(read_register(&bus, IRQ_CONTROLLER_MAGIC_OFFSET) ==
           IRQ_CONTROLLER_MAGIC);
    assert(read_register(&bus, IRQ_CONTROLLER_LINE_COUNT_OFFSET) ==
           INTERRUPT_EXTERNAL_LINE_COUNT);
    assert(read_register(&bus, IRQ_CONTROLLER_TARGET_COUNT_OFFSET) == 2);
    assert(read_register(&bus, IRQ_CONTROLLER_ENABLE_OFFSET) == 0);

    /* 마스킹된 IRQ는 controller pending에 함께 보존된다. */
    assert(interrupt_router_raise(&router, 7));
    assert(interrupt_router_raise(&router, 3));
    assert(read_register(&bus, IRQ_CONTROLLER_PENDING_OFFSET) ==
           ((UINT64_C(1) << 7) | (UINT64_C(1) << 3)));
    unsigned int line;
    assert(!interrupt_controller_take_next(&target0, &line));

    /* 같은 대상의 대기 IRQ 중 번호가 낮은 IRQ 3만 먼저 전달한다. */
    write_register(&bus,
                   IRQ_CONTROLLER_ENABLE_SET_OFFSET,
                   (UINT64_C(1) << 7) | (UINT64_C(1) << 3));
    assert(read_register(&bus, IRQ_CONTROLLER_ACTIVE_OFFSET) ==
           (UINT64_C(1) << 3));
    assert(read_register(&bus, IRQ_CONTROLLER_PENDING_OFFSET) ==
           (UINT64_C(1) << 7));
    /* IPI는 별도 high-priority class라 외부 IRQ보다 먼저 선택된다. */
    assert(interrupt_controller_raise(&target0,
                                      INTERRUPT_IPI_LINE_BASE));
    assert(interrupt_controller_take_next(&target0, &line));
    assert(line == INTERRUPT_IPI_LINE_BASE);
    assert(interrupt_controller_take_next(&target0, &line));
    assert(line == 3);

    /* active인 같은 IRQ는 재진입하지 않고 한 번의 pending으로 합친다. */
    assert(interrupt_router_raise(&router, 3));
    assert((read_register(&bus, IRQ_CONTROLLER_PENDING_OFFSET) &
            (UINT64_C(1) << 3)) != 0);
    write_register(&bus, IRQ_CONTROLLER_LINE_SELECT_OFFSET, 3);
    write_register(&bus, IRQ_CONTROLLER_ROUTE_OFFSET, 1);
    assert(read_register(&bus, IRQ_CONTROLLER_RESULT_OFFSET) ==
           IRQ_CONTROLLER_RESULT_BUSY);

    /* EOI 후 active 중 합쳐진 IRQ 3이 한 번 재전달된다. */
    write_register(&bus, IRQ_CONTROLLER_EOI_OFFSET, 3);
    assert(read_register(&bus, IRQ_CONTROLLER_RESULT_OFFSET) ==
           IRQ_CONTROLLER_RESULT_SUCCESS);
    assert(read_register(&bus, IRQ_CONTROLLER_ACTIVE_OFFSET) ==
           (UINT64_C(1) << 3));
    assert(interrupt_controller_take_next(&target0, &line));
    assert(line == 3);

    /* 두 번째 EOI 뒤 같은 대상에서 대기하던 IRQ 7이 전달된다. */
    write_register(&bus, IRQ_CONTROLLER_EOI_OFFSET, 3);
    assert(read_register(&bus, IRQ_CONTROLLER_ACTIVE_OFFSET) ==
           (UINT64_C(1) << 7));
    assert(interrupt_controller_take_next(&target0, &line));
    assert(line == 7);
    write_register(&bus, IRQ_CONTROLLER_EOI_OFFSET, 7);
    assert(read_register(&bus, IRQ_CONTROLLER_ACTIVE_OFFSET) == 0);

    /* IRQ 5를 논리 프로세서 1로 route하고 unmask하면 target1로 간다. */
    assert(interrupt_router_raise(&router, 5));
    write_register(&bus, IRQ_CONTROLLER_LINE_SELECT_OFFSET, 5);
    write_register(&bus, IRQ_CONTROLLER_ROUTE_OFFSET, 1);
    assert(read_register(&bus, IRQ_CONTROLLER_ROUTE_OFFSET) == 1);
    write_register(&bus,
                   IRQ_CONTROLLER_ENABLE_SET_OFFSET,
                   UINT64_C(1) << 5);
    assert(interrupt_controller_take_next(&target1, &line));
    assert(line == 5);
    assert(!interrupt_controller_take_next(&target0, &line));
    write_register(&bus, IRQ_CONTROLLER_EOI_OFFSET, 5);

    write_register(&bus,
                   IRQ_CONTROLLER_ENABLE_SET_OFFSET,
                   UINT64_C(1) << INTERRUPT_IPI_LINE_BASE);
    assert(read_register(&bus, IRQ_CONTROLLER_RESULT_OFFSET) ==
           IRQ_CONTROLLER_RESULT_INVALID_MASK);
    write_register(&bus,
                   IRQ_CONTROLLER_LINE_SELECT_OFFSET,
                   INTERRUPT_EXTERNAL_LINE_COUNT);
    assert(read_register(&bus, IRQ_CONTROLLER_RESULT_OFFSET) ==
           IRQ_CONTROLLER_RESULT_INVALID_LINE);
    write_register(&bus, IRQ_CONTROLLER_EOI_OFFSET, 5);
    assert(read_register(&bus, IRQ_CONTROLLER_RESULT_OFFSET) ==
           IRQ_CONTROLLER_RESULT_NOT_ACTIVE);

    bus_reset(&bus);
    assert(read_register(&bus, IRQ_CONTROLLER_ENABLE_OFFSET) == 0);
    assert(read_register(&bus, IRQ_CONTROLLER_PENDING_OFFSET) == 0);
    assert(read_register(&bus, IRQ_CONTROLLER_ACTIVE_OFFSET) == 0);
    assert(read_register(&bus, IRQ_CONTROLLER_RESULT_OFFSET) ==
           IRQ_CONTROLLER_RESULT_NONE);

    return 0;
}
