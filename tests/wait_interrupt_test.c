#include "bus.h"
#include "cpu.h"
#include "host_thread.h"
#include "interrupt.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>

#define VECTOR_TABLE_BASE UINT64_C(0x180)

typedef struct {
    VirtualMachine *vm;
    HardwareThread *target;
} InterruptRaiseContext;

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static int raise_when_waiting(void *context)
{
    InterruptRaiseContext *raise_context = context;

    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        if (atomic_load_explicit(&raise_context->target->state,
                                 memory_order_acquire) ==
            HARDWARE_THREAD_WAITING) {
            return interrupt_router_raise(
                &raise_context->vm->interrupt_router,
                TIMER_INTERRUPT_LINE);
        }
        host_thread_sleep_milliseconds(1);
    }

    return 0;
}

int test_wait_interrupt(void)
{
    uint8_t memory[1024] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    /* SP 초기화, IRQ 허용 후 WAIT한다. */
    memory[1] = OP_MOVI64;
    memory[2] = REGISTER_SP;
    write_u64_le(&memory[3], 0x3F0);
    memory[11] = OP_EI;
    memory[12] = OP_WAIT;

    /* IRET 후 WAIT 다음 주소에서 실행을 계속한다. */
    memory[13] = OP_MOVI64;
    memory[14] = 1;
    write_u64_le(&memory[15], 99);
    memory[23] = OP_HALT;

    /* IRQ handler: R0=42; IRET */
    memory[0x40] = OP_MOVI64;
    memory[0x41] = 0;
    write_u64_le(&memory[0x42], 42);
    memory[0x4A] = OP_IRET;

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 1, 1));
    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    boot->cpu.pc = 1;
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         TIMER_INTERRUPT_LINE * CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x40));
    assert(vm_set_vector_base(&vm, 0, VECTOR_TABLE_BASE));
    assert(vm_route_interrupt(&vm, TIMER_INTERRUPT_LINE, 0));

    InterruptRaiseContext raise_context = {
        .vm = &vm,
        .target = boot
    };
    HostThread raiser;
    assert(host_thread_create(&raiser,
                              raise_when_waiting,
                              &raise_context));

    assert(vm_run(&vm));

    int raiser_result = 0;
    assert(host_thread_join(&raiser, &raiser_result));
    assert(raiser_result);
    assert(boot->cpu.registers[0] == 42);
    assert(boot->cpu.registers[1] == 99);
    assert(boot->cpu.registers[REGISTER_SP] == 0x3F0);
    assert(!boot->cpu.waiting);
    assert(boot->cpu.halted);
    assert(atomic_load(&boot->state) == HARDWARE_THREAD_HALTED);

    vm_destroy(&vm);
    return 0;
}
