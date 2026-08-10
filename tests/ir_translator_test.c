#include "assembler.h"
#include "bus.h"
#include "cpu.h"
#include "cvmir.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *ir_make_raw_source(const char *assembly, const char *entry_setup)
{
    size_t capacity = strlen(entry_setup) + strlen(assembly) + 1;
    char *raw = malloc(capacity);
    assert(raw != NULL);
    strcpy(raw, entry_setup);
    size_t length = strlen(raw);
    const char *line = assembly;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t line_length = end != NULL ? (size_t)(end - line + 1)
                                         : strlen(line);
        int metadata = strncmp(line, ".section ", 9) == 0 ||
                       strncmp(line, ".global ", 8) == 0 ||
                       strncmp(line, ".type ", 6) == 0 ||
                       strncmp(line, ".size ", 6) == 0 ||
                       strncmp(line, ".weak ", 6) == 0 ||
                       strncmp(line, ".extern ", 8) == 0;
        if (!metadata) {
            memcpy(raw + length, line, line_length);
            length += line_length;
        }
        line += line_length;
    }
    raw[length] = '\0';
    return raw;
}

int test_ir_translator(void)
{
    static const char source[] =
        "target datalayout = \"" CVM_LLVM_DATA_LAYOUT "\"\n"
        "target triple = \"" CVM_LLVM_TARGET_TRIPLE "\"\n"
        "define i64 @add(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %sum = add nsw i64 %a, %b\n"
        "  ret i64 %sum\n"
        "}\n"
        "define i64 @checked_add(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %sum = tail call i64 @add(i64 %a, i64 %b)\n"
        "  %okay = icmp eq i64 %sum, 42\n"
        "  %chosen = select i1 %okay, i64 %sum, i64 0\n"
        "  br i1 %okay, label %yes, label %no\n"
        "yes:\n"
        "  ret i64 %chosen\n"
        "no:\n"
        "  ret i64 0\n"
        "}\n";
    CvmIrOptions options = {0};
    CvmIrError error;
    char *assembly = NULL;
    assert(cvmir_translate(source, &options, &assembly, &error));
    assert(strstr(assembly, ".global checked_add") != NULL);
    assert(strstr(assembly, "CALLREL add") != NULL);
    assert(strstr(assembly, "CMP R0, R1") != NULL);
    assert(strstr(assembly, "CMPI32 R0, 0") != NULL);

    static const char arithmetic_entry[] =
        "MOVI64 SP, 8192\n"
        "MOVI64 R0, 19\n"
        "MOVI64 R1, 23\n"
        "CALL checked_add\n"
        "HALT\n";
    char *raw_source = ir_make_raw_source(assembly, arithmetic_entry);
    AssemblyResult binary;
    AssemblyError assembly_error;
    assert(assembler_assemble(raw_source, 1, &binary, &assembly_error));
    assert(binary.size < 8191);
    uint8_t memory[8192] = {0};
    memcpy(memory + 1, binary.data, binary.size);
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    size_t steps = 0;
    while (!cpu.halted) {
        assert(++steps < 10000);
        assert(cpu_step(&cpu, &bus));
    }
    assert(cpu.registers[0] == 42);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assembly_result_destroy(&binary);
    free(raw_source);
    free(assembly);

    static const char foreign[] =
        "target triple = \"x86_64-unknown-linux-gnu\"\n"
        "define i64 @value(i64 %x) { ret i64 %x }\n";
    assembly = NULL;
    assert(!cvmir_translate(foreign, &options, &assembly, &error));
    assert(strstr(error.message, "target triple") != NULL);

    static const char memory_source[] =
        "target datalayout = \"" CVM_LLVM_DATA_LAYOUT "\"\n"
        "target triple = \"" CVM_LLVM_TARGET_TRIPLE "\"\n"
        "@kernel_result = global i64 0, align 8\n"
        "define i64 @memory_test() {\n"
        "entry:\n"
        "  %array = alloca [3 x i64], align 8\n"
        "  %p0 = getelementptr inbounds [3 x i64], ptr %array, i64 0, i64 0\n"
        "  %p1 = getelementptr inbounds [3 x i64], ptr %array, i64 0, i64 1\n"
        "  %p2 = getelementptr inbounds [3 x i64], ptr %array, i64 0, i64 2\n"
        "  store i64 10, ptr %p0, align 8\n"
        "  store i64 12, ptr %p1, align 8\n"
        "  store i64 20, ptr %p2, align 8\n"
        "  br label %loop\n"
        "loop:\n"
        "  %i = phi i64 [ 0, %entry ], [ %next, %body ]\n"
        "  %sum = phi i64 [ 0, %entry ], [ %newsum, %body ]\n"
        "  %more = icmp ult i64 %i, 3\n"
        "  br i1 %more, label %body, label %exit\n"
        "body:\n"
        "  %p = getelementptr inbounds [3 x i64], ptr %array, i64 0, i64 %i\n"
        "  %value = load i64, ptr %p, align 8\n"
        "  %newsum = add i64 %sum, %value\n"
        "  %next = add i64 %i, 1\n"
        "  br label %loop\n"
        "exit:\n"
        "  %absolute.slot = alloca ptr, align 8\n"
        "  store ptr inttoptr (i64 4096 to ptr), ptr %absolute.slot, align 8\n"
        "  %absolute.ptr = load ptr, ptr %absolute.slot, align 8\n"
        "  %absolute = ptrtoint ptr %absolute.ptr to i64\n"
        "  %absolute.ok = icmp eq i64 %absolute, 4096\n"
        "  %checked.sum = select i1 %absolute.ok, i64 %sum, i64 0\n"
        "  store i64 %checked.sum, ptr @kernel_result, align 8\n"
        "  %small = trunc i64 %checked.sum to i32\n"
        "  %wide = zext i32 %small to i64\n"
        "  ret i64 %wide\n"
        "}\n";
    assert(cvmir_translate(memory_source, &options, &assembly, &error));
    assert(strstr(assembly, "LEA R0, R14") != NULL);
    assert(strstr(assembly, "STORE64 R1, R0") != NULL);
    assert(strstr(assembly, ".section .bss") != NULL);
    static const char memory_entry[] =
        "MOVI64 SP, 8192\n"
        "CALL memory_test\n"
        "HALT\n";
    raw_source = ir_make_raw_source(assembly, memory_entry);
    assert(assembler_assemble(raw_source, 1, &binary, &assembly_error));
    assert(binary.size < sizeof(memory) - 1);
    memset(memory, 0, sizeof(memory));
    memcpy(memory + 1, binary.data, binary.size);
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    steps = 0;
    while (!cpu.halted) {
        assert(++steps < 10000);
        assert(cpu_step(&cpu, &bus));
    }
    assert(cpu.registers[0] == 42);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assembly_result_destroy(&binary);
    free(raw_source);
    free(assembly);

    static const char stack_source[] =
        "target datalayout = \"" CVM_LLVM_DATA_LAYOUT "\"\n"
        "target triple = \"" CVM_LLVM_TARGET_TRIPLE "\"\n"
        "define i64 @sum10(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, "
        "i64 %f, i64 %g, i64 %h, i64 %i, i64 %j) {\n"
        "entry:\n"
        "  %s1 = add i64 %a, %b\n"
        "  %s2 = add i64 %s1, %c\n"
        "  %s3 = add i64 %s2, %d\n"
        "  %s4 = add i64 %s3, %e\n"
        "  %s5 = add i64 %s4, %f\n"
        "  %s6 = add i64 %s5, %g\n"
        "  %s7 = add i64 %s6, %h\n"
        "  %s8 = add i64 %s7, %i\n"
        "  %s9 = add i64 %s8, %j\n"
        "  ret i64 %s9\n"
        "}\n"
        "define i64 @varfirst(i64 %value, ...) {\n"
        "entry:\n"
        "  ret i64 %value\n"
        "}\n"
        "define i64 @stack_test() {\n"
        "entry:\n"
        "  %sum = call i64 @sum10(i64 1, i64 2, i64 3, i64 4, i64 5, "
        "i64 6, i64 7, i64 8, i64 9, i64 10)\n"
        "  %ignored = call i64 (i64, ...) @varfirst(i64 0, i64 99)\n"
        "  %result = add i64 %sum, %ignored\n"
        "  ret i64 %result\n"
        "}\n";
    assert(cvmir_translate(stack_source, &options, &assembly, &error));
    assert(strstr(assembly, "LOAD64O R0, R14, 16") != NULL);
    static const char stack_entry[] =
        "MOVI64 SP, 8192\n"
        "CALL stack_test\n"
        "HALT\n";
    raw_source = ir_make_raw_source(assembly, stack_entry);
    assert(assembler_assemble(raw_source, 1, &binary, &assembly_error));
    assert(binary.size < sizeof(memory) - 1);
    memset(memory, 0, sizeof(memory));
    memcpy(memory + 1, binary.data, binary.size);
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    steps = 0;
    while (!cpu.halted) {
        assert(++steps < 10000);
        assert(cpu_step(&cpu, &bus));
    }
    assert(cpu.registers[0] == 55);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assembly_result_destroy(&binary);
    free(raw_source);
    free(assembly);

    static const char intrinsic_source[] =
        "target datalayout = \"" CVM_LLVM_DATA_LAYOUT "\"\n"
        "target triple = \"" CVM_LLVM_TARGET_TRIPLE "\"\n"
        "declare void @cvm_disable_interrupts()\n"
        "declare void @cvm_enable_interrupts()\n"
        "declare void @cvm_fence()\n"
        "declare i64 @cvm_core_id()\n"
        "declare i64 @cvm_thread_id()\n"
        "declare i64 @cvm_get_mmu()\n"
        "declare i64 @cvm_xchg64(ptr, i64)\n"
        "define i64 @intrinsic_test() {\n"
        "entry:\n"
        "  %cell = alloca i64, align 8\n"
        "  store i64 42, ptr %cell, align 8\n"
        "  call void @cvm_disable_interrupts()\n"
        "  call void @cvm_fence()\n"
        "  %old = call i64 @cvm_xchg64(ptr %cell, i64 99)\n"
        "  %core = call i64 @cvm_core_id()\n"
        "  %thread = call i64 @cvm_thread_id()\n"
        "  %mmu = call i64 @cvm_get_mmu()\n"
        "  %sum1 = add i64 %old, %core\n"
        "  %sum2 = add i64 %sum1, %thread\n"
        "  %sum3 = add i64 %sum2, %mmu\n"
        "  call void @cvm_enable_interrupts()\n"
        "  ret i64 %sum3\n"
        "}\n";
    assert(cvmir_translate(intrinsic_source, &options, &assembly, &error));
    assert(strstr(assembly, "CALLREL cvm_") == NULL);
    assert(strstr(assembly, ".extern cvm_") == NULL);
    assert(strstr(assembly, "    DI\n") != NULL);
    assert(strstr(assembly, "    EI\n") != NULL);
    assert(strstr(assembly, "    FENCE\n") != NULL);
    assert(strstr(assembly, "    XCHG64 R0, R1\n") != NULL);
    assert(strstr(assembly, "    COREID R0\n") != NULL);
    static const char intrinsic_entry[] =
        "MOVI64 SP, 8192\n"
        "CALL intrinsic_test\n"
        "HALT\n";
    raw_source = ir_make_raw_source(assembly, intrinsic_entry);
    assert(assembler_assemble(raw_source, 1, &binary, &assembly_error));
    assert(binary.size < sizeof(memory) - 1);
    memset(memory, 0, sizeof(memory));
    memcpy(memory + 1, binary.data, binary.size);
    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    steps = 0;
    while (!cpu.halted) {
        assert(++steps < 10000);
        assert(cpu_step(&cpu, &bus));
    }
    assert(cpu.registers[0] == 42);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assembly_result_destroy(&binary);
    free(raw_source);
    free(assembly);
    return 0;
}
