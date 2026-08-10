#include "bus.h"
#include "cpu.h"
#include "device_manager.h"
#include "interrupt.h"
#include "irq_controller.h"
#include "isa.h"
#include "mmu.h"
#include "ram.h"
#include "system_control.h"
#include "system_info.h"
#include "timer.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static void emit_movi64(uint8_t *memory,
                        size_t *cursor,
                        uint8_t destination,
                        uint64_t value)
{
    memory[(*cursor)++] = OP_MOVI64;
    memory[(*cursor)++] = destination;
    write_u64_le(&memory[*cursor], value);
    *cursor += 8;
}

static uint64_t read_register(Bus *bus, uint64_t base, uint64_t offset)
{
    uint64_t value = UINT64_MAX;
    assert(bus_read(bus, base + offset, sizeof(value), &value));
    return value;
}

int test_system_devices(void)
{
    uint8_t memory[512] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 2, 2));

    SystemInfoDevice configuration = {
        .features = SYSTEM_INFO_FEATURE_MMU |
                    SYSTEM_INFO_FEATURE_MULTICORE |
                    SYSTEM_INFO_FEATURE_INTERRUPTS |
                    SYSTEM_INFO_FEATURE_ATOMICS |
                    SYSTEM_INFO_FEATURE_SIMD |
                    SYSTEM_INFO_FEATURE_FLOATING_POINT |
                    SYSTEM_INFO_FEATURE_BOOT_ROM |
                    SYSTEM_INFO_FEATURE_VIO |
                    SYSTEM_INFO_FEATURE_SYSTEM_CONTROL,
        .ram_base = 0,
        .ram_size = sizeof(memory),
        .rom_base = UINT64_C(0x7FFFF00000),
        .rom_size = UINT64_C(0x2000),
        .reset_vector = 1,
        .core_count = 2,
        .threads_per_core = 2,
        .logical_processor_count = 4,
        .physical_address_bits = 64,
        .page_size = MMU_PAGE_SIZE,
        .virtual_address_bits = MMU_VIRTUAL_ADDRESS_BITS,
        .timer_frequency = TIMER_TICKS_PER_SECOND,
        .interrupt_line_count = INTERRUPT_LINE_COUNT,
        .vio_hub_base = VIO_HUB_MMIO_BASE,
        .vio_slot_count = DEVICE_MANAGER_MAX_SLOTS,
        .dynamic_mmio_base = DEVICE_MANAGER_DYNAMIC_MMIO_BASE,
        .dynamic_mmio_size = DEVICE_MANAGER_DYNAMIC_MMIO_LIMIT -
                             DEVICE_MANAGER_DYNAMIC_MMIO_BASE + 1,
        .system_control_base = SYSTEM_CONTROL_MMIO_BASE,
        .external_interrupt_line_count =
            INTERRUPT_EXTERNAL_LINE_COUNT,
        .ipi_line_base = INTERRUPT_IPI_LINE_BASE,
        .irq_controller_base = IRQ_CONTROLLER_MMIO_BASE
    };
    SystemInfoDevice information;
    assert(system_info_device_init(&information, &configuration));
    assert(bus_map_device(&bus,
                          SYSTEM_INFO_MMIO_BASE,
                          SYSTEM_INFO_MMIO_SIZE,
                          system_info_device_as_bus_device(&information)));

    SystemControlDevice control;
    assert(system_control_device_init(&control, &vm));
    assert(bus_map_device(&bus,
                          SYSTEM_CONTROL_MMIO_BASE,
                          SYSTEM_CONTROL_MMIO_SIZE,
                          system_control_device_as_bus_device(&control)));

    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_MAGIC_OFFSET) == SYSTEM_INFO_MAGIC);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_RAM_SIZE_OFFSET) == sizeof(memory));
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_CORE_COUNT_OFFSET) == 2);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_LOGICAL_PROCESSORS_OFFSET) == 4);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_PAGE_SIZE_OFFSET) == MMU_PAGE_SIZE);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_CONTROL_BASE_OFFSET) ==
           SYSTEM_CONTROL_MMIO_BASE);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_EXTERNAL_IRQ_COUNT_OFFSET) ==
           INTERRUPT_EXTERNAL_LINE_COUNT);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_IPI_BASE_OFFSET) ==
           INTERRUPT_IPI_LINE_BASE);
    assert(read_register(&bus,
                         SYSTEM_INFO_MMIO_BASE,
                         SYSTEM_INFO_IRQ_CONTROLLER_BASE_OFFSET) ==
           IRQ_CONTROLLER_MMIO_BASE);
    assert(!bus_write(&bus,
                      SYSTEM_INFO_MMIO_BASE + SYSTEM_INFO_RAM_SIZE_OFFSET,
                      8,
                      1));
    uint64_t value;
    assert(!bus_read(&bus,
                     SYSTEM_INFO_MMIO_BASE + SYSTEM_INFO_RAM_SIZE_OFFSET,
                     4,
                     &value));

    /* 첫 실행에서 reset vector를 HALT로 바꾼 뒤 warm reset을 요청한다. */
    size_t cursor = 1;
    emit_movi64(memory, &cursor, 0, 1);
    emit_movi64(memory, &cursor, 1, OP_HALT);
    memory[cursor++] = OP_STORE8;
    memory[cursor++] = 0;
    memory[cursor++] = 1;
    emit_movi64(memory,
                &cursor,
                0,
                SYSTEM_CONTROL_MMIO_BASE +
                    SYSTEM_CONTROL_COMMAND_OFFSET);
    emit_movi64(memory,
                &cursor,
                1,
                SYSTEM_CONTROL_COMMAND_WARM_RESET);
    memory[cursor++] = OP_STORE64;
    memory[cursor++] = 0;
    memory[cursor++] = 1;
    memory[cursor++] = OP_HALT;

    assert(vm_set_reset_vector(&vm, 1));
    assert(vm_run(&vm));
    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    assert(atomic_load(&boot->state) == HARDWARE_THREAD_HALTED);
    assert(vm_system_action(&vm) == VM_SYSTEM_ACTION_NONE);
    assert(read_register(&bus,
                         SYSTEM_CONTROL_MMIO_BASE,
                         SYSTEM_CONTROL_RESET_CAUSE_OFFSET) ==
           SYSTEM_CONTROL_RESET_CAUSE_SOFTWARE);
    assert(read_register(&bus,
                         SYSTEM_CONTROL_MMIO_BASE,
                         SYSTEM_CONTROL_RESET_COUNT_OFFSET) == 1);
    assert(read_register(&bus,
                         SYSTEM_CONTROL_MMIO_BASE,
                         SYSTEM_CONTROL_STATUS_OFFSET) ==
           SYSTEM_CONTROL_STATUS_RUNNING);

    assert(bus_write(&bus,
                     SYSTEM_CONTROL_MMIO_BASE +
                         SYSTEM_CONTROL_COMMAND_OFFSET,
                     8,
                     UINT64_C(0xFFFF)));
    assert(read_register(&bus,
                         SYSTEM_CONTROL_MMIO_BASE,
                         SYSTEM_CONTROL_RESULT_OFFSET) ==
           SYSTEM_CONTROL_RESULT_INVALID_COMMAND);
    assert(vm_system_action(&vm) == VM_SYSTEM_ACTION_NONE);

    assert(bus_write(&bus,
                     SYSTEM_CONTROL_MMIO_BASE +
                         SYSTEM_CONTROL_COMMAND_OFFSET,
                     8,
                     SYSTEM_CONTROL_COMMAND_SHUTDOWN));
    assert(vm_system_action(&vm) == VM_SYSTEM_ACTION_SHUTDOWN);
    assert(read_register(&bus,
                         SYSTEM_CONTROL_MMIO_BASE,
                         SYSTEM_CONTROL_STATUS_OFFSET) ==
           SYSTEM_CONTROL_STATUS_SHUTDOWN_PENDING);
    assert(read_register(&bus,
                         SYSTEM_CONTROL_MMIO_BASE,
                         SYSTEM_CONTROL_RESULT_OFFSET) ==
           SYSTEM_CONTROL_RESULT_ACCEPTED);
    assert(!bus_write(&bus,
                      SYSTEM_CONTROL_MMIO_BASE +
                          SYSTEM_CONTROL_STATUS_OFFSET,
                      8,
                      0));

    vm_destroy(&vm);
    return 0;
}
