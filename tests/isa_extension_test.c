#include "bus.h"
#include "cpu.h"
#include "ram.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static void isa_emit8(uint8_t *memory,
                      size_t capacity,
                      size_t *cursor,
                      uint8_t value)
{
    assert(*cursor < capacity);
    memory[(*cursor)++] = value;
}

static void isa_emit32(uint8_t *memory,
                       size_t capacity,
                       size_t *cursor,
                       uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) {
        isa_emit8(memory,
                  capacity,
                  cursor,
                  (uint8_t)(value >> (i * 8)));
    }
}

static void isa_emit64(uint8_t *memory,
                       size_t capacity,
                       size_t *cursor,
                       uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        isa_emit8(memory,
                  capacity,
                  cursor,
                  (uint8_t)(value >> (i * 8)));
    }
}

static void isa_emit_reg_imm32(uint8_t *memory,
                               size_t capacity,
                               size_t *cursor,
                               uint8_t opcode,
                               uint8_t reg,
                               uint32_t immediate)
{
    isa_emit8(memory, capacity, cursor, opcode);
    isa_emit8(memory, capacity, cursor, reg);
    isa_emit32(memory, capacity, cursor, immediate);
}

static void isa_emit_movi64(uint8_t *memory,
                            size_t capacity,
                            size_t *cursor,
                            uint8_t reg,
                            uint64_t immediate)
{
    isa_emit8(memory, capacity, cursor, OP_MOVI64);
    isa_emit8(memory, capacity, cursor, reg);
    isa_emit64(memory, capacity, cursor, immediate);
}

static void isa_emit_register_pair(uint8_t *memory,
                                   size_t capacity,
                                   size_t *cursor,
                                   uint8_t opcode,
                                   uint8_t first,
                                   uint8_t second)
{
    isa_emit8(memory, capacity, cursor, opcode);
    isa_emit8(memory, capacity, cursor, first);
    isa_emit8(memory, capacity, cursor, second);
}

static void isa_emit_offset(uint8_t *memory,
                            size_t capacity,
                            size_t *cursor,
                            uint8_t opcode,
                            uint8_t first,
                            uint8_t second,
                            int32_t displacement)
{
    isa_emit_register_pair(memory,
                           capacity,
                           cursor,
                           opcode,
                           first,
                           second);
    isa_emit32(memory,
               capacity,
               cursor,
               (uint32_t)displacement);
}

static void isa_run(CPU *cpu, uint8_t *memory, size_t size)
{
    RAM ram = {
        .data = memory,
        .size = size
    };
    Bus bus;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(cpu, &ram));
    cpu->pc = 1;

    size_t steps = 0;
    while (!cpu->halted) {
        assert(++steps < 1000);
        assert(cpu_step(cpu, &bus));
    }
}

static void test_immediate_and_lea(void)
{
    uint8_t memory[512] = {0};
    size_t cursor = 1;

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       0,
                       UINT32_MAX);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32S,
                       1,
                       UINT32_MAX);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       2,
                       100);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_ADDI32,
                       2,
                       (uint32_t)-5);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_CMPI32,
                       2,
                       95);

    isa_emit8(memory, sizeof(memory), &cursor, OP_BRCC);
    isa_emit8(memory, sizeof(memory), &cursor, CPU_CONDITION_EQ);
    isa_emit32(memory, sizeof(memory), &cursor, 6);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       14,
                       1);

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_ANDI32,
                       0,
                       0xFF);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_ORI32,
                       0,
                       0x100);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_XORI32,
                       0,
                       0x0F);

    isa_emit8(memory, sizeof(memory), &cursor, OP_LEA);
    isa_emit8(memory, sizeof(memory), &cursor, 3);
    isa_emit8(memory, sizeof(memory), &cursor, 2);
    isa_emit32(memory, sizeof(memory), &cursor, (uint32_t)-5);

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_TESTI32,
                       0,
                       0);
    isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

    CPU cpu;
    isa_run(&cpu, memory, sizeof(memory));
    assert(cpu.registers[0] == UINT64_C(0x1F0));
    assert(cpu.registers[1] == UINT64_MAX);
    assert(cpu.registers[2] == 95);
    assert(cpu.registers[3] == 90);
    assert(cpu.registers[14] == 0);
    assert((cpu.flags & CPU_FLAG_ZERO) != 0);
}

static void test_offset_memory(void)
{
    uint8_t memory[1024] = {0};
    size_t cursor = 1;
    uint64_t stored = UINT64_C(0x88776655F4338280);

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       0,
                       0x310);
    isa_emit_movi64(memory, sizeof(memory), &cursor, 1, stored);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_STORE64O,
                    0,
                    1,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD64O,
                    2,
                    0,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD8UO,
                    3,
                    0,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD8SO,
                    4,
                    0,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD16UO,
                    5,
                    0,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD16SO,
                    6,
                    0,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD32UO,
                    7,
                    0,
                    -13);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_LOAD32SO,
                    8,
                    0,
                    -13);

    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_STORE8O,
                    0,
                    1,
                    -20);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_STORE16O,
                    0,
                    1,
                    -18);
    isa_emit_offset(memory,
                    sizeof(memory),
                    &cursor,
                    OP_STORE32O,
                    0,
                    1,
                    -16);
    isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

    CPU cpu;
    isa_run(&cpu, memory, sizeof(memory));
    assert(cpu.registers[2] == stored);
    assert(cpu.registers[3] == UINT64_C(0x80));
    assert(cpu.registers[4] == UINT64_C(0xFFFFFFFFFFFFFF80));
    assert(cpu.registers[5] == UINT64_C(0x8280));
    assert(cpu.registers[6] == UINT64_C(0xFFFFFFFFFFFF8280));
    assert(cpu.registers[7] == UINT64_C(0xF4338280));
    assert(cpu.registers[8] == UINT64_C(0xFFFFFFFFF4338280));
    assert(memory[0x2FC] == 0x80);
    assert(memory[0x2FE] == 0x80 && memory[0x2FF] == 0x82);
    assert(memory[0x300] == 0x80 && memory[0x303] == 0xF4);
}

static void test_shift_and_extend(void)
{
    uint8_t memory[512] = {0};
    size_t cursor = 1;

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       0,
                       1);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       1,
                       65);
    isa_emit_register_pair(memory,
                           sizeof(memory),
                           &cursor,
                           OP_SHLV,
                           0,
                           1);
    isa_emit_movi64(memory,
                    sizeof(memory),
                    &cursor,
                    2,
                    UINT64_C(0x8000000000000000));
    isa_emit_register_pair(memory,
                           sizeof(memory),
                           &cursor,
                           OP_SHRV,
                           2,
                           1);
    isa_emit_movi64(memory,
                    sizeof(memory),
                    &cursor,
                    3,
                    UINT64_C(0x8000000000000000));
    isa_emit_register_pair(memory,
                           sizeof(memory),
                           &cursor,
                           OP_SARV,
                           3,
                           1);

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       4,
                       UINT32_C(0xF4338280));
    const uint8_t extension_opcodes[6] = {
        OP_SEXT8, OP_SEXT16, OP_SEXT32,
        OP_ZEXT8, OP_ZEXT16, OP_ZEXT32
    };
    for (uint8_t i = 0; i < 6; ++i) {
        isa_emit_register_pair(memory,
                               sizeof(memory),
                               &cursor,
                               extension_opcodes[i],
                               (uint8_t)(5 + i),
                               4);
    }
    isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

    CPU cpu;
    isa_run(&cpu, memory, sizeof(memory));
    assert(cpu.registers[0] == 2);
    assert(cpu.registers[2] == UINT64_C(0x4000000000000000));
    assert(cpu.registers[3] == UINT64_C(0xC000000000000000));
    assert(cpu.registers[5] == UINT64_C(0xFFFFFFFFFFFFFF80));
    assert(cpu.registers[6] == UINT64_C(0xFFFFFFFFFFFF8280));
    assert(cpu.registers[7] == UINT64_C(0xFFFFFFFFF4338280));
    assert(cpu.registers[8] == UINT64_C(0x80));
    assert(cpu.registers[9] == UINT64_C(0x8280));
    assert(cpu.registers[10] == UINT64_C(0xF4338280));
}

static void test_indirect_and_relative_control(void)
{
    uint8_t memory[512] = {0};
    size_t cursor = 1;

    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       REGISTER_SP,
                       sizeof(memory));
    isa_emit8(memory, sizeof(memory), &cursor, OP_CALLREL);
    isa_emit32(memory, sizeof(memory), &cursor, 88);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       2,
                       120);
    isa_emit8(memory, sizeof(memory), &cursor, OP_CALLR);
    isa_emit8(memory, sizeof(memory), &cursor, 2);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       3,
                       140);
    isa_emit8(memory, sizeof(memory), &cursor, OP_JUMPR);
    isa_emit8(memory, sizeof(memory), &cursor, 3);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       12,
                       1);
    isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

    cursor = 100;
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       10,
                       11);
    isa_emit8(memory, sizeof(memory), &cursor, OP_RET);

    cursor = 120;
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       11,
                       22);
    isa_emit8(memory, sizeof(memory), &cursor, OP_RET);

    cursor = 140;
    isa_emit8(memory, sizeof(memory), &cursor, OP_JUMPREL);
    isa_emit32(memory, sizeof(memory), &cursor, 15);
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       12,
                       2);
    isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

    cursor = 160;
    isa_emit_reg_imm32(memory,
                       sizeof(memory),
                       &cursor,
                       OP_MOVI32U,
                       13,
                       33);
    isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

    CPU cpu;
    isa_run(&cpu, memory, sizeof(memory));
    assert(cpu.registers[10] == 11);
    assert(cpu.registers[11] == 22);
    assert(cpu.registers[12] == 0);
    assert(cpu.registers[13] == 33);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
}

static int isa_condition_expected(uint8_t condition, uint64_t flags)
{
    int zero = (flags & CPU_FLAG_ZERO) != 0;
    int negative = (flags & CPU_FLAG_NEGATIVE) != 0;
    int carry = (flags & CPU_FLAG_CARRY) != 0;
    int overflow = (flags & CPU_FLAG_OVERFLOW) != 0;
    int unordered = (flags & CPU_FLAG_UNORDERED) != 0;
    switch ((CPUCondition)condition) {
        case CPU_CONDITION_ALWAYS: return 1;
        case CPU_CONDITION_EQ: return zero;
        case CPU_CONDITION_NE: return !zero;
        case CPU_CONDITION_LT: return !unordered && negative != overflow;
        case CPU_CONDITION_LE:
            return !unordered && (zero || negative != overflow);
        case CPU_CONDITION_GT:
            return !unordered && !zero && negative == overflow;
        case CPU_CONDITION_GE: return !unordered && negative == overflow;
        case CPU_CONDITION_LTU: return !carry;
        case CPU_CONDITION_LEU: return !carry || zero;
        case CPU_CONDITION_GTU: return carry && !zero;
        case CPU_CONDITION_GEU: return carry;
        case CPU_CONDITION_ORDERED: return !unordered;
        case CPU_CONDITION_UNORDERED: return unordered;
        case CPU_CONDITION_COUNT: return 0;
    }
    return 0;
}

static void test_brcc_conditions(void)
{
    const uint64_t flag_cases[] = {
        0,
        CPU_FLAG_ZERO | CPU_FLAG_CARRY,
        CPU_FLAG_NEGATIVE,
        CPU_FLAG_NEGATIVE | CPU_FLAG_OVERFLOW | CPU_FLAG_CARRY,
        CPU_FLAG_UNORDERED
    };
    for (size_t flags_index = 0;
         flags_index < sizeof(flag_cases) / sizeof(flag_cases[0]);
         ++flags_index) {
        for (uint8_t condition = 0;
             condition < CPU_CONDITION_COUNT;
             ++condition) {
            uint8_t memory[64] = {0};
            size_t cursor = 1;
            isa_emit8(memory, sizeof(memory), &cursor, OP_BRCC);
            isa_emit8(memory, sizeof(memory), &cursor, condition);
            isa_emit32(memory, sizeof(memory), &cursor, 7);
            isa_emit_reg_imm32(memory,
                               sizeof(memory),
                               &cursor,
                               OP_MOVI32U,
                               0,
                               0);
            isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);
            isa_emit_reg_imm32(memory,
                               sizeof(memory),
                               &cursor,
                               OP_MOVI32U,
                               0,
                               1);
            isa_emit8(memory, sizeof(memory), &cursor, OP_HALT);

            RAM ram = {
                .data = memory,
                .size = sizeof(memory)
            };
            Bus bus;
            CPU cpu;
            assert(bus_init(&bus, &ram));
            assert(cpu_init(&cpu, &ram));
            cpu.pc = 1;
            cpu.flags = flag_cases[flags_index];
            while (!cpu.halted) {
                assert(cpu_step(&cpu, &bus));
            }
            assert(cpu.registers[0] ==
                   (uint64_t)isa_condition_expected(
                       condition,
                       flag_cases[flags_index]));
        }
    }
}

int test_isa_extension(void)
{
    test_immediate_and_lea();
    test_offset_memory();
    test_shift_and_extend();
    test_indirect_and_relative_control();
    test_brcc_conditions();
    return 0;
}
