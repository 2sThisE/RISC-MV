#include "bus.h"
#include "cpu.h"
#include "interrupt.h"
#include "ram.h"
#include "timer.h"
#include "vm_clock.h"

#include <assert.h>
#include <stdint.h>

#define TIMER_TEST_BASE UINT64_C(0xFFFFFFFFFFFF0000)
#define VECTOR_TABLE_BASE UINT64_C(0x180)

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

int test_interrupt(void)
{
    uint8_t memory[1024] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };

    /* VM 코드가 MMIO로 compare=4를 설정한다. */
    memory[1] = OP_MOVI64;
    memory[2] = 1;
    write_u64_le(&memory[3], TIMER_TEST_BASE + TIMER_COMPARE_OFFSET);
    memory[11] = OP_MOVI64;
    memory[12] = 2;
    write_u64_le(&memory[13], 4);
    memory[21] = OP_STORE64;
    memory[22] = 1;
    memory[23] = 2;

    /* 타이머와 IRQ를 활성화한 뒤 4 tick째에 IRQ를 받는다. */
    memory[24] = OP_MOVI64;
    memory[25] = 1;
    write_u64_le(&memory[26], TIMER_TEST_BASE + TIMER_CONTROL_OFFSET);
    memory[34] = OP_MOVI64;
    memory[35] = 2;
    write_u64_le(&memory[36],
                 TIMER_CONTROL_ENABLE | TIMER_CONTROL_IRQ_ENABLE);
    memory[44] = OP_STORE64;
    memory[45] = 1;
    memory[46] = 2;
    memory[47] = OP_EI;
    memory[48] = OP_NOP;
    memory[49] = OP_NOP;

    /* 복귀 후 IRQ를 끄고 MMIO에서 pending 상태를 R3으로 읽는다. */
    memory[50] = OP_DI;
    memory[51] = OP_MOVI64;
    memory[52] = 1;
    write_u64_le(&memory[53], TIMER_TEST_BASE + TIMER_STATUS_OFFSET);
    memory[61] = OP_LOAD64;
    memory[62] = 3;
    memory[63] = 1;
    memory[64] = OP_HALT;

    /* 0x80: MOVI64 R0, 42; IRET */
    memory[0x80] = OP_MOVI64;
    memory[0x81] = 0;
    write_u64_le(&memory[0x82], 42);
    memory[0x8A] = OP_IRET;

    CPU cpu;
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    cpu.registers[REGISTER_SP] = sizeof(memory);

    InterruptController interrupts;
    interrupt_controller_init(&interrupts);
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         TIMER_INTERRUPT_LINE * CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x80));
    assert(cpu_set_vector_base(&cpu, &ram, VECTOR_TABLE_BASE));

    Bus bus;
    assert(bus_init(&bus, &ram));

    TimerDevice timer;
    assert(timer_device_init(&timer, TIMER_INTERRUPT_LINE));
    assert(bus_map_device(&bus,
                          TIMER_TEST_BASE,
                          TIMER_MMIO_SIZE,
                          timer_device_as_bus_device(&timer)));

    InstructionClock instruction_clock;
    ClockSource clock;
    assert(instruction_clock_init(&instruction_clock,
                                  &clock,
                                  1));

    assert(cpu_run(&cpu, &bus, &interrupts, &clock));
    assert(cpu.registers[0] == 42);
    assert(cpu.registers[3] == TIMER_STATUS_PENDING);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assert((cpu.flags & CPU_FLAG_INTERRUPT_ENABLE) == 0);

    uint64_t timer_status;
    assert(bus_read(&bus,
                    TIMER_TEST_BASE + TIMER_STATUS_OFFSET,
                    8,
                    &timer_status));
    assert((timer_status & TIMER_STATUS_PENDING) != 0);

    assert(bus_write(&bus,
                     TIMER_TEST_BASE + TIMER_STATUS_OFFSET,
                     8,
                     TIMER_STATUS_PENDING));
    assert(bus_read(&bus,
                    TIMER_TEST_BASE + TIMER_STATUS_OFFSET,
                    8,
                    &timer_status));
    assert(timer_status == 0);
    return 0;
}
