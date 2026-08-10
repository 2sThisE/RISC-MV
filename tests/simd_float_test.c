#include "assembler.h"
#include "bus.h"
#include "cpu.h"
#include "ram.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static uint64_t vector_lane(const CPUVectorRegister *reg,
                            size_t lane,
                            size_t width)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        value |= (uint64_t)reg->bytes[lane * width + i] << (i * 8);
    }
    return value;
}

static void memory_lane(uint8_t *memory,
                        size_t address,
                        size_t width,
                        uint64_t value)
{
    for (size_t i = 0; i < width; ++i) {
        memory[address + i] = (uint8_t)(value >> (i * 8));
    }
}

static void memory_float32(uint8_t *memory, size_t address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    memory_lane(memory, address, 4, bits);
}

static void memory_float64(uint8_t *memory, size_t address, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    memory_lane(memory, address, 8, bits);
}

static double memory_float64_value(const uint8_t *memory, size_t address)
{
    uint64_t bits = 0;
    for (size_t i = 0; i < 8; ++i) {
        bits |= (uint64_t)memory[address + i] << (i * 8);
    }
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float register_float32(const CPUVectorRegister *reg, size_t lane)
{
    uint32_t bits = (uint32_t)vector_lane(reg, lane, 4);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double register_float64(const CPUVectorRegister *reg, size_t lane)
{
    uint64_t bits = vector_lane(reg, lane, 8);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void assemble_and_run(const char *source,
                             uint8_t *memory,
                             size_t memory_size,
                             CPU *cpu)
{
    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));
    assert(result.size <= memory_size - 1);
    memcpy(memory + 1, result.data, result.size);

    RAM ram = {
        .data = memory,
        .size = memory_size
    };
    Bus bus;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(cpu, &ram));
    cpu->pc = 1;
    cpu->registers[0] = 513;
    cpu->registers[1] = 547;

    size_t steps = 0;
    while (!cpu->halted) {
        assert(++steps < 500);
        assert(cpu_step(cpu, &bus));
    }
    assembly_result_destroy(&result);
}

static uint64_t width_mask(size_t width)
{
    return width == 8
               ? UINT64_MAX
               : (UINT64_C(1) << (width * 8)) - 1;
}

static void assert_integer_result(const CPUVectorRegister *actual,
                                  const uint8_t left[16],
                                  const uint8_t right[16],
                                  size_t width,
                                  char operation)
{
    CPUVectorRegister left_register;
    CPUVectorRegister right_register;
    memcpy(left_register.bytes, left, 16);
    memcpy(right_register.bytes, right, 16);
    for (size_t lane = 0; lane < 16 / width; ++lane) {
        uint64_t a = vector_lane(&left_register, lane, width);
        uint64_t b = vector_lane(&right_register, lane, width);
        uint64_t expected = operation == '+'
                                ? a + b
                                : operation == '-'
                                      ? a - b
                                      : a * b;
        expected &= width_mask(width);
        assert(vector_lane(actual, lane, width) == expected);
    }
}

static void test_integer_simd(void)
{
    static const char source[] =
        "VLOAD128 V0, R0\n"
        "VLOAD128 V1, R1\n"
        "VADD8 V2, V0, V1\n"
        "VADD16 V3, V0, V1\n"
        "VADD32 V4, V0, V1\n"
        "VADD64 V5, V0, V1\n"
        "VSUB8 V6, V0, V1\n"
        "VSUB16 V7, V0, V1\n"
        "VSUB32 V8, V0, V1\n"
        "VSUB64 V9, V0, V1\n"
        "VMUL8 V10, V0, V1\n"
        "VMUL16 V11, V0, V1\n"
        "VMUL32 V12, V0, V1\n"
        "VMUL64 V13, V0, V1\n"
        "HALT\n";
    uint8_t memory[1024] = {0};
    uint8_t left[16];
    uint8_t right[16];
    for (size_t i = 0; i < 16; ++i) {
        left[i] = (uint8_t)(i * 17 + 3);
        right[i] = (uint8_t)(i * 7 + 1);
    }
    memcpy(memory + 513, left, 16);
    memcpy(memory + 547, right, 16);

    CPU cpu;
    assemble_and_run(source, memory, sizeof(memory), &cpu);
    assert(memcmp(cpu.vector_registers[0].bytes, left, 16) == 0);
    assert(memcmp(cpu.vector_registers[1].bytes, right, 16) == 0);
    assert_integer_result(&cpu.vector_registers[2], left, right, 1, '+');
    assert_integer_result(&cpu.vector_registers[3], left, right, 2, '+');
    assert_integer_result(&cpu.vector_registers[4], left, right, 4, '+');
    assert_integer_result(&cpu.vector_registers[5], left, right, 8, '+');
    assert_integer_result(&cpu.vector_registers[6], left, right, 1, '-');
    assert_integer_result(&cpu.vector_registers[7], left, right, 2, '-');
    assert_integer_result(&cpu.vector_registers[8], left, right, 4, '-');
    assert_integer_result(&cpu.vector_registers[9], left, right, 8, '-');
    assert_integer_result(&cpu.vector_registers[10], left, right, 1, '*');
    assert_integer_result(&cpu.vector_registers[11], left, right, 2, '*');
    assert_integer_result(&cpu.vector_registers[12], left, right, 4, '*');
    assert_integer_result(&cpu.vector_registers[13], left, right, 8, '*');
}

static void test_vector_logic_and_store(void)
{
    static const char source[] =
        "VLOAD128 V0, R0\n"
        "VLOAD128 V1, R1\n"
        "VAND V2, V0, V1\n"
        "VOR V3, V0, V1\n"
        "VXOR V4, V0, V1\n"
        "VMOV V5, V4\n"
        "VSTORE128 R2, V5\n"
        "HALT\n";
    uint8_t memory[1024] = {0};
    uint8_t left[16];
    uint8_t right[16];
    for (size_t i = 0; i < 16; ++i) {
        left[i] = (uint8_t)(0xF0U + i);
        right[i] = (uint8_t)(0x0FU + i);
    }
    memcpy(memory + 513, left, 16);
    memcpy(memory + 547, right, 16);

    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));
    memcpy(memory + 1, result.data, result.size);
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    cpu.registers[0] = 513;
    cpu.registers[1] = 547;
    cpu.registers[2] = 581;
    while (!cpu.halted) assert(cpu_step(&cpu, &bus));
    for (size_t i = 0; i < 16; ++i) {
        assert(cpu.vector_registers[2].bytes[i] == (uint8_t)(left[i] & right[i]));
        assert(cpu.vector_registers[3].bytes[i] == (uint8_t)(left[i] | right[i]));
        assert(cpu.vector_registers[4].bytes[i] == (uint8_t)(left[i] ^ right[i]));
        assert(cpu.vector_registers[5].bytes[i] == cpu.vector_registers[4].bytes[i]);
        assert(memory[581 + i] == cpu.vector_registers[4].bytes[i]);
    }
    assembly_result_destroy(&result);
}

static void test_scalar_float(void)
{
    static const char source[] =
        "FLOAD32 V0, R0\n"
        "FLOAD32 V1, R1\n"
        "FADD32 V2, V0, V1\n"
        "FSUB32 V3, V0, V1\n"
        "FMUL32 V4, V0, V1\n"
        "FDIV32 V5, V0, V1\n"
        "FNEG32 V6, V0\n"
        "FABS32 V7, V6\n"
        "FCMP32 V0, V1\n"
        "I32TOF32 V8, R4\n"
        "F32TOI32 R5, V8\n"
        "F32TOF64 V9, V0\n"
        "F64TOF32 V10, V9\n"
        "FSTORE32 R2, V2\n"
        "FLOAD32 V11, R3\n"
        "FDIV32 V12, V0, V11\n"
        "GETFPSTATUS R6\n"
        "HALT\n";
    uint8_t memory[1024] = {0};
    memory_float32(memory, 600, 7.5f);
    memory_float32(memory, 604, 2.5f);
    memory_float32(memory, 608, 0.0f);

    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));
    memcpy(memory + 1, result.data, result.size);
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    cpu.registers[0] = 600;
    cpu.registers[1] = 604;
    cpu.registers[2] = 612;
    cpu.registers[3] = 608;
    cpu.registers[4] = (uint64_t)(int64_t)-7;
    while (!cpu.halted) assert(cpu_step(&cpu, &bus));

    assert(register_float32(&cpu.vector_registers[2], 0) == 10.0f);
    assert(register_float32(&cpu.vector_registers[3], 0) == 5.0f);
    assert(register_float32(&cpu.vector_registers[4], 0) == 18.75f);
    assert(register_float32(&cpu.vector_registers[5], 0) == 3.0f);
    assert(register_float32(&cpu.vector_registers[6], 0) == -7.5f);
    assert(register_float32(&cpu.vector_registers[7], 0) == 7.5f);
    assert(register_float32(&cpu.vector_registers[8], 0) == -7.0f);
    assert(cpu.registers[5] == UINT64_MAX - 6);
    assert(register_float64(&cpu.vector_registers[9], 0) == 7.5);
    assert(register_float32(&cpu.vector_registers[10], 0) == 7.5f);
    assert(isinf(register_float32(&cpu.vector_registers[12], 0)));
    assert((cpu.registers[6] & CPU_FP_DIVIDE_BY_ZERO) != 0);
    assert((cpu.flags & CPU_FLAG_NEGATIVE) == 0);
    assert(memory[612] == 0 && memory[613] == 0 &&
           memory[614] == 0x20 && memory[615] == 0x41);
    for (size_t i = 4; i < 16; ++i) {
        assert(cpu.vector_registers[2].bytes[i] == 0);
    }
    assembly_result_destroy(&result);
}

static void test_double_and_vector_float(void)
{
    static const char source[] =
        "FLOAD64 V0, R0\n"
        "FLOAD64 V1, R1\n"
        "FADD64 V2, V0, V1\n"
        "FSUB64 V3, V0, V1\n"
        "FMUL64 V4, V0, V1\n"
        "FDIV64 V5, V0, V1\n"
        "FNEG64 V6, V0\n"
        "FABS64 V7, V6\n"
        "FCMP64 V0, V1\n"
        "I64TOF64 V8, R4\n"
        "F64TOI64 R5, V8\n"
        "FSTORE64 R2, V4\n"
        "FSTORE64 R8, V2\n"
        "FSTORE64 R9, V3\n"
        "FSTORE64 R10, V4\n"
        "FSTORE64 R11, V5\n"
        "VLOAD128 V9, R6\n"
        "VLOAD128 V10, R7\n"
        "VFADD32 V11, V9, V10\n"
        "VFSUB32 V12, V10, V9\n"
        "VFMUL32 V13, V9, V10\n"
        "VFDIV32 V14, V10, V9\n"
        "VFADD64 V15, V9, V10\n"
        "VFSUB64 V2, V10, V9\n"
        "VFMUL64 V3, V9, V10\n"
        "VFDIV64 V4, V10, V9\n"
        "HALT\n";
    uint8_t memory[1200] = {0};
    memory_float64(memory, 700, 9.0);
    memory_float64(memory, 708, 3.0);
    const float f32_left[4] = {1.0f, 2.0f, 4.0f, 8.0f};
    const float f32_right[4] = {10.0f, 20.0f, 40.0f, 80.0f};
    for (size_t i = 0; i < 4; ++i) {
        memory_float32(memory, 800 + i * 4, f32_left[i]);
        memory_float32(memory, 816 + i * 4, f32_right[i]);
    }

    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));
    memcpy(memory + 1, result.data, result.size);
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    cpu.registers[0] = 700;
    cpu.registers[1] = 708;
    cpu.registers[2] = 716;
    cpu.registers[4] = (uint64_t)(int64_t)-42;
    cpu.registers[6] = 800;
    cpu.registers[7] = 816;
    cpu.registers[8] = 720;
    cpu.registers[9] = 728;
    cpu.registers[10] = 736;
    cpu.registers[11] = 744;
    while (!cpu.halted) assert(cpu_step(&cpu, &bus));

    assert(register_float64(&cpu.vector_registers[5], 0) == 3.0);
    assert(register_float64(&cpu.vector_registers[6], 0) == -9.0);
    assert(register_float64(&cpu.vector_registers[7], 0) == 9.0);
    assert(register_float64(&cpu.vector_registers[8], 0) == -42.0);
    assert(cpu.registers[5] == (uint64_t)(int64_t)-42);
    assert(memory_float64_value(memory, 720) == 12.0);
    assert(memory_float64_value(memory, 728) == 6.0);
    assert(memory_float64_value(memory, 736) == 27.0);
    assert(memory_float64_value(memory, 744) == 3.0);
    for (size_t lane = 0; lane < 4; ++lane) {
        assert(register_float32(&cpu.vector_registers[11], lane) ==
               f32_left[lane] + f32_right[lane]);
        assert(register_float32(&cpu.vector_registers[12], lane) ==
               f32_right[lane] - f32_left[lane]);
        assert(register_float32(&cpu.vector_registers[13], lane) ==
               f32_left[lane] * f32_right[lane]);
        assert(register_float32(&cpu.vector_registers[14], lane) ==
               f32_right[lane] / f32_left[lane]);
    }
    for (size_t lane = 0; lane < 2; ++lane) {
        double left = register_float64(&cpu.vector_registers[9], lane);
        double right = register_float64(&cpu.vector_registers[10], lane);
        assert(register_float64(&cpu.vector_registers[15], lane) == left + right);
        assert(register_float64(&cpu.vector_registers[2], lane) == right - left);
        assert(register_float64(&cpu.vector_registers[3], lane) == left * right);
        assert(register_float64(&cpu.vector_registers[4], lane) == right / left);
    }
    assembly_result_destroy(&result);
}

static void test_unordered_and_diagnostics(void)
{
    static const char source[] =
        "FLOAD32 V0, R0\n"
        "FCMP32 V0, V0\n"
        "BRCC UNO, unordered\n"
        "MOVI32U R1, 0\n"
        "HALT\n"
        "unordered: MOVI32U R1, 1\n"
        "GETFPSTATUS R2\n"
        "CLEARFPSTATUS\n"
        "GETFPSTATUS R3\n"
        "HALT\n";
    uint8_t memory[512] = {0};
    memory_lane(memory, 400, 4, UINT32_C(0x7FC00000));

    AssemblyResult result;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &result, &error));
    memcpy(memory + 1, result.data, result.size);
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    cpu.registers[0] = 400;
    while (!cpu.halted) assert(cpu_step(&cpu, &bus));
    assert(cpu.registers[1] == 1);
    assert((cpu.registers[2] & CPU_FP_INVALID) != 0);
    assert(cpu.registers[3] == 0);
    assert((cpu.flags & CPU_FLAG_UNORDERED) != 0);
    assembly_result_destroy(&result);

    assert(!assembler_assemble("VMOV V16, V0\n", 1, &result, &error));
    assembly_result_destroy(&result);
    assert(!assembler_assemble("VADD32 V0, R1, V2\n", 1, &result, &error));
    assembly_result_destroy(&result);
}

static void test_vector_access_fault_is_precise(void)
{
    uint8_t memory[1024] = {0};
    const uint64_t vbr = 128;
    const uint64_t handler = 300;
    size_t vector_entry = (size_t)vbr +
                          (CPU_VECTOR_EXCEPTION_BASE +
                           CPU_EXCEPTION_DATA_ACCESS) *
                              CPU_VECTOR_ENTRY_SIZE;
    memory_lane(memory, vector_entry, 8, handler);
    memory[handler] = OP_HALT;

    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    assert(cpu_set_vector_base(&cpu, &ram, vbr));
    cpu.pc = 1;
    cpu.registers[0] = sizeof(memory) - 8;
    cpu.registers[REGISTER_SP] = 900;
    memset(cpu.vector_registers[0].bytes, 0xA5, VECTOR_REGISTER_SIZE);
    memset(memory + sizeof(memory) - 8, 0xCC, 8);
    memory[1] = OP_VSTORE128;
    memory[2] = 0;
    memory[3] = 0;
    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == handler);
    assert(cpu.ecause == CPU_EXCEPTION_DATA_ACCESS);
    for (size_t i = sizeof(memory) - 8; i < sizeof(memory); ++i) {
        assert(memory[i] == 0xCC);
    }

    assert(cpu_init(&cpu, &ram));
    assert(cpu_set_vector_base(&cpu, &ram, vbr));
    cpu.pc = 1;
    cpu.registers[0] = sizeof(memory) - 8;
    cpu.registers[REGISTER_SP] = 900;
    memset(cpu.vector_registers[0].bytes, 0x5A, VECTOR_REGISTER_SIZE);
    memory[1] = OP_VLOAD128;
    memory[2] = 0;
    memory[3] = 0;
    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == handler);
    assert(cpu.ecause == CPU_EXCEPTION_DATA_ACCESS);
    for (size_t i = 0; i < VECTOR_REGISTER_SIZE; ++i) {
        assert(cpu.vector_registers[0].bytes[i] == 0x5A);
    }
}

int test_simd_float(void)
{
    test_integer_simd();
    test_vector_logic_and_store();
    test_scalar_float();
    test_double_and_vector_float();
    test_unordered_and_diagnostics();
    test_vector_access_fault_is_precise();
    return 0;
}
