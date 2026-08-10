#include "bus.h"
#include "cpu.h"
#include "irq_controller.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>

#define VECTOR_TABLE_BASE UINT64_C(0x180)

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

int test_vm_interrupt(void)
{
    uint8_t memory[1024] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    memory[1] = OP_EI;
    memory[2] = OP_NOP;
    memory[3] = OP_DI;
    memory[4] = OP_HALT;

    memory[0x40] = OP_MOVI64;
    memory[0x41] = 0;
    write_u64_le(&memory[0x42], 42);
    memory[0x4A] = OP_MOVI64;
    memory[0x4B] = 1;
    write_u64_le(&memory[0x4C],
                 IRQ_CONTROLLER_MMIO_BASE + IRQ_CONTROLLER_EOI_OFFSET);
    memory[0x54] = OP_MOVI64;
    memory[0x55] = 2;
    write_u64_le(&memory[0x56], TIMER_INTERRUPT_LINE);
    memory[0x5E] = OP_STORE64;
    memory[0x5F] = 1;
    memory[0x60] = 2;
    memory[0x61] = OP_IRET;

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 2, 2));
    IrqControllerDevice irq_controller;
    assert(irq_controller_device_init(&irq_controller,
                                      &vm.interrupt_router));
    assert(bus_map_device(
        &bus,
        IRQ_CONTROLLER_MMIO_BASE,
        IRQ_CONTROLLER_MMIO_SIZE,
        irq_controller_device_as_bus_device(&irq_controller)));
    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    boot->cpu.pc = 1;

    /* logical processor 3 = Core 1, Hardware Thread 1 */
    assert(vm_activate_hardware_thread(&vm,
                                       1,
                                       1,
                                       1));
    HardwareThread *irq_target = vm_hardware_thread(&vm, 1, 1);
    assert(irq_target != NULL);
    irq_target->cpu.registers[REGISTER_SP] = 0x3F0;
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         TIMER_INTERRUPT_LINE * CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x40));
    assert(vm_set_vector_base(&vm, 3, VECTOR_TABLE_BASE));
    assert(vm_route_interrupt(&vm, TIMER_INTERRUPT_LINE, 3));
    assert(bus_write(&bus,
                     IRQ_CONTROLLER_MMIO_BASE +
                         IRQ_CONTROLLER_ENABLE_SET_OFFSET,
                     8,
                     UINT64_C(1) << TIMER_INTERRUPT_LINE));
    assert(interrupt_router_raise(&vm.interrupt_router,
                                  TIMER_INTERRUPT_LINE));

    assert(vm_run(&vm));

    for (size_t core = 0; core < 2; ++core) {
        for (size_t thread = 0; thread < 2; ++thread) {
            HardwareThread *target = vm_hardware_thread(&vm,
                                                        core,
                                                        thread);
            assert(target != NULL);
            uint64_t expected = core == 1 && thread == 1 ? 42 : 0;
            assert(target->cpu.registers[0] == expected);
        }
    }
    assert(interrupt_router_active(&vm.interrupt_router) == 0);

    vm_destroy(&vm);
    return 0;
}
