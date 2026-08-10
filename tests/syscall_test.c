#include "assembler.h"
#include "bus.h"
#include "cpu.h"
#include "mmu.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint64_t syscall_symbol(const AssemblyResult *result,
                               const char *name)
{
    for (size_t i = 0; i < result->symbol_count; ++i) {
        if (strcmp(result->symbols[i].name, name) == 0) {
            return result->symbols[i].address;
        }
    }
    assert(!"expected syscall test symbol");
    return 0;
}

static uint64_t read_u64_le(const uint8_t *source)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= (uint64_t)source[i] << (i * 8);
    }
    return value;
}

static void test_user_syscall_round_trip(void)
{
    static const char source[] =
        ".entry start\n"
        "start:\n"
        "  MOVI32U SP, 0x300\n"
        "  MOVI32U R10, 0x3F0\n"
        "  SETKSP R10\n"
        "  MOVI32U R11, 0x400\n"
        "  SETVBR R11\n"
        "  MOVI32U R0, 7\n"
        "  MOVI32U R1, 41\n"
        "  ENTERUSER\n"
        "  SYSCALL\n"
        "after_syscall:\n"
        "  NOP\n"
        ".org 0x100\n"
        "syscall_handler:\n"
        "  GETMODE R2\n"
        "  GETKSP R3\n"
        "  MOV R4, SP\n"
        "  MOV R5, R0\n"
        "  MOV R0, R1\n"
        "  ADDI32 R0, 1\n"
        "  IRET\n"
        ".org 0x660\n"
        "  .qword syscall_handler\n";

    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));

    uint8_t memory[2048] = {0};
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

    uint64_t return_address = syscall_symbol(&result, "after_syscall");
    size_t steps = 0;
    do {
        assert(++steps < 50);
        assert(cpu_step(&cpu, &bus));
    } while (cpu.mode != CPU_MODE_USER || cpu.pc != return_address);

    assert(cpu.registers[0] == 42);
    assert(cpu.registers[1] == 41);
    assert(cpu.registers[2] == CPU_MODE_SUPERVISOR);
    assert(cpu.registers[3] == 0x3F0);
    assert(cpu.registers[4] == 0x3D8);
    assert(cpu.registers[5] == 7);
    assert(cpu.registers[REGISTER_SP] == 0x300);
    assert(cpu.ksp == 0x3F0);
    assert(read_u64_le(&memory[0x3D8]) == return_address);
    assert(read_u64_le(&memory[0x3E8]) == 0x300);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == return_address + 1);
    assembly_result_destroy(&result);
}

static void test_setksp_is_privileged(void)
{
    uint8_t memory[1024] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));

    memory[1] = OP_SETKSP;
    memory[2] = 0;
    memory[0x80] = OP_IRET;
    assert(ram_write(&ram,
                     0x180 +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_PRIVILEGE_VIOLATION) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x80));
    assert(cpu_set_vector_base(&cpu, &ram, 0x180));

    cpu.pc = 1;
    cpu.mode = CPU_MODE_USER;
    cpu.registers[0] = 0x200;
    cpu.registers[REGISTER_SP] = 0x280;
    cpu.ksp = 0x300;
    assert(cpu_step(&cpu, &bus));
    assert(cpu.mode == CPU_MODE_SUPERVISOR);
    assert(cpu.pc == 0x80);
    assert(cpu.ecause == CPU_EXCEPTION_PRIVILEGE_VIOLATION);
    assert(cpu.ksp == 0x300);
    assert(cpu.registers[REGISTER_SP] == 0x2E8);
}

static void test_syscall_uses_supervisor_ksp_mapping(void)
{
    enum { RAM_SIZE = 65536 };
    static uint8_t memory[RAM_SIZE];
    memset(memory, 0, sizeof(memory));
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));

    const uint64_t root = 0xD000;
    const uint64_t level1 = 0xE000;
    const uint64_t level0 = 0xF000;
    const uint64_t vector_base = 0xA000;
    assert(ram_write(&ram, root, 8, level1 | MMU_PTE_VALID));
    assert(ram_write(&ram, level1, 8, level0 | MMU_PTE_VALID));
    assert(ram_write(&ram,
                     level0,
                     8,
                     MMU_PTE_VALID | MMU_PTE_READ | MMU_PTE_WRITE |
                         MMU_PTE_EXECUTE | MMU_PTE_USER));
    assert(ram_write(&ram,
                     level0 + 8,
                     8,
                     UINT64_C(0x1000) | MMU_PTE_VALID |
                         MMU_PTE_READ | MMU_PTE_EXECUTE));
    assert(ram_write(&ram,
                     level0 + 24,
                     8,
                     UINT64_C(0x3000) | MMU_PTE_VALID |
                         MMU_PTE_READ | MMU_PTE_WRITE));

    memory[1] = OP_SYSCALL;
    memory[2] = OP_NOP;
    memory[0x1000] = OP_IRET;
    assert(ram_write(&ram,
                     vector_base + CPU_VECTOR_SYSCALL *
                                       CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x1000));

    assert(cpu_init(&cpu, &ram));
    assert(cpu_set_vector_base(&cpu, &ram, vector_base));
    cpu.ptbr = root;
    cpu.ptbr_set = 1;
    cpu.mmu_enabled = 1;
    cpu.mode = CPU_MODE_USER;
    cpu.pc = 1;
    cpu.registers[REGISTER_SP] = 0x800;
    cpu.ksp = 0x3800;

    assert(cpu_step(&cpu, &bus));
    assert(cpu.mode == CPU_MODE_SUPERVISOR);
    assert(cpu.pc == 0x1000);
    assert(cpu.registers[REGISTER_SP] == 0x37E8);
    assert(read_u64_le(&memory[0x37E8]) == 2);
    assert(read_u64_le(&memory[0x37F8]) == 0x800);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.mode == CPU_MODE_USER);
    assert(cpu.pc == 2);
    assert(cpu.registers[REGISTER_SP] == 0x800);
    assert(cpu.ksp == 0x3800);
}

int test_syscall(void)
{
    test_user_syscall_round_trip();
    test_setksp_is_privileged();
    test_syscall_uses_supervisor_ksp_mapping();
    return 0;
}
