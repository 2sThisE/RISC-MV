#include "assembler.h"
#include "bus.h"
#include "cpu.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t find_symbol(const AssemblyResult *result, const char *name)
{
    for (size_t i = 0; i < result->symbol_count; ++i) {
        if (strcmp(result->symbols[i].name, name) == 0) {
            return result->symbols[i].address;
        }
    }
    assert(!"expected assembler symbol");
    return 0;
}

static void test_assembled_program_executes(void)
{
    static const char source[] =
        "; forward and backward relative references\n"
        ".entry start\n"
        "start:\n"
        "  MOVI32U SP, 512\n"
        "  MOVI32U R0, 5\n"
        "loop:\n"
        "  ADDI32 R0, -1\n"
        "  CMPI32 R0, 0\n"
        "  BRCC NE, loop\n"
        "  CALLREL set_result\n"
        "  HALT\n"
        "set_result:\n"
        "  MOVI32U R1, 42\n"
        "  RET\n"
        ".align 8\n"
        "message:\n"
        "  .ascii \"OK\"\n"
        "  .byte 0\n";

    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));
    assert(result.entry_set);
    assert(result.entry_address == find_symbol(&result, "start"));
    assert(find_symbol(&result, "loop") < find_symbol(&result, "set_result"));

    uint64_t message_address = find_symbol(&result, "message");
    size_t message_offset = (size_t)(message_address - result.base_address);
    assert(message_offset + 3 <= result.size);
    assert(memcmp(result.data + message_offset, "OK\0", 3) == 0);

    uint8_t memory[512] = {0};
    assert(result.size <= sizeof(memory) - result.base_address);
    memcpy(memory + result.base_address, result.data, result.size);
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = result.entry_address;

    size_t steps = 0;
    while (!cpu.halted) {
        assert(++steps < 100);
        assert(cpu_step(&cpu, &bus));
    }
    assert(cpu.registers[0] == 0);
    assert(cpu.registers[1] == 42);
    assert(cpu.registers[REGISTER_SP] == 512);
    assembly_result_destroy(&result);
}

static void test_directive_layout(void)
{
    static const char source[] =
        ".byte 0x12\n"
        ".align 4, 0xAA\n"
        "aligned: .word 0x3456\n"
        ".space 2, 0xCC\n"
        ".zero 3\n"
        ".org 0x10\n"
        ".dword 0x89ABCDEF\n"
        ".qword -1\n";
    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 0, &result, &error));
    assert(result.size == 28);
    assert(result.data[0] == 0x12);
    assert(result.data[1] == 0xAA);
    assert(result.data[2] == 0xAA);
    assert(result.data[3] == 0xAA);
    assert(find_symbol(&result, "aligned") == 4);
    assert(result.data[4] == 0x56 && result.data[5] == 0x34);
    assert(result.data[6] == 0xCC && result.data[7] == 0xCC);
    for (size_t i = 8; i < 16; ++i) {
        assert(result.data[i] == 0);
    }
    assert(result.data[16] == 0xEF && result.data[19] == 0x89);
    for (size_t i = 20; i < 28; ++i) {
        assert(result.data[i] == 0xFF);
    }
    assembly_result_destroy(&result);
}

static void test_equ_and_expression_precedence(void)
{
    static const char source[] =
        ".equ VALUE, 2 + 3 * 4\n"
        ".set MASK, (1 << 5) | 3\n"
        ".qword VALUE, MASK, ~0 & 0xFF, 17 / 4, 17 % 4\n";
    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 0, &result, &error));
    assert(result.size == 40);
    const uint64_t expected[] = {14, 35, 255, 4, 1};
    for (size_t item = 0; item < 5; ++item) {
        uint64_t value = 0;
        for (size_t byte = 0; byte < 8; ++byte)
            value |= (uint64_t)result.data[item * 8 + byte] << (byte * 8);
        assert(value == expected[item]);
    }
    assembly_result_destroy(&result);
}

static void expect_assembly_failure(const char *source)
{
    AssemblyResult result;
    AssemblyError error;
    assert(!assembler_assemble(source, 1, &result, &error));
    assert(error.line != 0);
    assert(error.column != 0);
    assert(error.message[0] != '\0');
    assembly_result_destroy(&result);
}

static void test_diagnostics(void)
{
    expect_assembly_failure("same: NOP\nsame: HALT\n");
    expect_assembly_failure("JUMPREL nowhere\n");
    expect_assembly_failure("SHL R0, 64\n");
    expect_assembly_failure(".ascii \"\\x1\"\n");
    expect_assembly_failure("MOVI32U R16, 1\n");
    expect_assembly_failure(".qword 1 / 0\n");
    expect_assembly_failure(".qword 1 << 64\n");
}

int test_assembler(void)
{
    test_assembled_program_executes();
    test_directive_layout();
    test_equ_and_expression_precedence();
    test_diagnostics();
    return 0;
}
