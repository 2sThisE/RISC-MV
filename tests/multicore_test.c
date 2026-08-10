#include "bus.h"
#include "cpu.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>

#define TEST_CORE_COUNT 4U
#define TEST_THREADS_PER_CORE 2U
#define TEST_ITERATIONS UINT64_C(10000)
#define SHARED_COUNTER_ADDRESS UINT64_C(0x100)

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

int test_multicore(void)
{
    uint8_t memory[4096] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    /* R0=counter, R1=1, R4=iterations, R5=1 */
    memory[1] = OP_MOVI64;
    memory[2] = 0;
    write_u64_le(&memory[3], SHARED_COUNTER_ADDRESS);
    memory[11] = OP_MOVI64;
    memory[12] = 1;
    write_u64_le(&memory[13], 1);
    memory[21] = OP_MOVI64;
    memory[22] = 4;
    write_u64_le(&memory[23], TEST_ITERATIONS);
    memory[31] = OP_MOVI64;
    memory[32] = 5;
    write_u64_le(&memory[33], 1);

    /* loop: atomic_fetch_add(counter, 1); --R4; if (R4 != 0) loop */
    memory[41] = OP_ATOMIC_ADD64;
    memory[42] = 0;
    memory[43] = 1;
    memory[44] = 6;
    memory[45] = OP_SUB;
    memory[46] = 4;
    memory[47] = 5;
    memory[48] = OP_JNZ;
    write_u64_le(&memory[49], 41);

    memory[57] = OP_COREID;
    memory[58] = 2;
    memory[59] = OP_THREADID;
    memory[60] = 3;
    memory[61] = OP_HALT;

    assert(ram_write(&ram, SHARED_COUNTER_ADDRESS, 8, 0));

    VirtualMachine vm;
    assert(vm_init(&vm,
                   &ram,
                   &bus,
                   TEST_CORE_COUNT,
                   TEST_THREADS_PER_CORE));

    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    boot->cpu.pc = 1;

    for (size_t logical_index = 1;
         logical_index < TEST_CORE_COUNT * TEST_THREADS_PER_CORE;
         ++logical_index) {
        HardwareThread *offline = vm_hardware_thread(
            &vm,
            logical_index / TEST_THREADS_PER_CORE,
            logical_index % TEST_THREADS_PER_CORE);
        assert(offline != NULL);
        assert(atomic_load(&offline->state) == HARDWARE_THREAD_OFFLINE);
        assert(offline->cpu.registers[REGISTER_SP] == 0);
    }

    /* 추가 하드웨어 스레드는 PC만 지정해 활성화한다. */
    for (size_t core = 0; core < TEST_CORE_COUNT; ++core) {
        for (size_t thread = 0;
             thread < TEST_THREADS_PER_CORE;
             ++thread) {
            if (core == 0 && thread == 0) {
                continue;
            }
            assert(vm_activate_hardware_thread(&vm,
                                               core,
                                               thread,
                                               1));
        }
    }
    assert(vm_run(&vm));

    uint64_t counter;
    assert(ram_read(&ram, SHARED_COUNTER_ADDRESS, 8, &counter));
    assert(counter == TEST_CORE_COUNT *
                      TEST_THREADS_PER_CORE *
                      TEST_ITERATIONS);

    for (size_t core = 0; core < TEST_CORE_COUNT; ++core) {
        for (size_t thread = 0;
             thread < TEST_THREADS_PER_CORE;
             ++thread) {
            HardwareThread *hardware_thread =
                vm_hardware_thread(&vm, core, thread);
            assert(hardware_thread != NULL);
            assert(hardware_thread->cpu.registers[2] == core);
            assert(hardware_thread->cpu.registers[3] == thread);
            assert(hardware_thread->cpu.halted);
            assert(atomic_load(&hardware_thread->state) ==
                   HARDWARE_THREAD_HALTED);
        }
    }

    vm_destroy(&vm);
    return 0;
}
