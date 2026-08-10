#include "bus.h"
#include "cpu.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

int test_stack(void)
{
    uint8_t memory[256] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    memory[1] = OP_MOVI64;
    memory[2] = REGISTER_SP;
    write_u64_le(&memory[3], sizeof(memory));
    memory[11] = OP_MOV;
    memory[12] = 1;
    memory[13] = REGISTER_SP;

    memory[14] = OP_MOVI64;
    memory[15] = 2;
    write_u64_le(&memory[16], 42);
    memory[24] = OP_PUSH;
    memory[25] = 2;
    memory[26] = OP_POP;
    memory[27] = 3;

    memory[28] = OP_CALL;
    write_u64_le(&memory[29], 40);
    memory[37] = OP_HALT;

    memory[40] = OP_MOVI64;
    memory[41] = 4;
    write_u64_le(&memory[42], 99);
    memory[50] = OP_RET;

    CPU cpu;
    assert(cpu_init(&cpu, &ram));
    assert(cpu.registers[REGISTER_SP] == 0);
    cpu.pc = 1;

    while (!cpu.halted) {
        assert(cpu_step(&cpu, &bus));
    }

    assert(cpu.registers[1] == sizeof(memory));
    assert(cpu.registers[3] == 42);
    assert(cpu.registers[4] == 99);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    return 0;
}
