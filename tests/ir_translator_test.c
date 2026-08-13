#include "assembler.h"
#include "bus.h"
#include "cpu.h"
#include "cvmir.h"
#include "object_assembler.h"
#include "ram.h"

#include <assert.h>
#include <stdio.h>
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
        "target triple = \"" RARCH_M64_LLVM_TARGET_TRIPLE "\"\n"
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
        "target triple = \"" RARCH_M64_LLVM_TARGET_TRIPLE "\"\n"
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
        "target triple = \"" RARCH_M64_LLVM_TARGET_TRIPLE "\"\n"
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
        "target triple = \"" RARCH_M64_LLVM_TARGET_TRIPLE "\"\n"
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

    static const char p2a_source[] =
        "target datalayout = \"" CVM_LLVM_DATA_LAYOUT "\"\n"
        "target triple = \"" RARCH_M64_LLVM_TARGET_TRIPLE "\"\n"
        "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n"
        "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n"
        "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)\n"
        "define i64 @fn_a(i64 %x) {\n"
        "entry:\n"
        "  %r = add i64 %x, 10\n"
        "  ret i64 %r\n"
        "}\n"
        "define i64 @fn_b(i64 %x) {\n"
        "entry:\n"
        "  %r = add i64 %x, 20\n"
        "  ret i64 %r\n"
        "}\n"
        "define i64 @switch_eval(i32 %code) {\n"
        "entry:\n"
        "  switch i32 %code, label %sw.default [\n"
        "    i32 1, label %sw.bb1\n"
        "    i32 2, label %sw.bb2\n"
        "  ]\n"
        "sw.bb1:\n"
        "  ret i64 100\n"
        "sw.bb2:\n"
        "  ret i64 200\n"
        "sw.default:\n"
        "  ret i64 300\n"
        "}\n"
        "define i64 @p2a_test() {\n"
        "entry:\n"
        "  %fn_ptr_a = alloca ptr, align 8\n"
        "  %fn_ptr_b = alloca ptr, align 8\n"
        "  store ptr @fn_a, ptr %fn_ptr_a, align 8\n"
        "  store ptr @fn_b, ptr %fn_ptr_b, align 8\n"
        "  %fp_a = load ptr, ptr %fn_ptr_a, align 8\n"
        "  %res_a = call i64 %fp_a(i64 5)\n"
        "  %fp_b = load ptr, ptr %fn_ptr_b, align 8\n"
        "  %res_b = call i64 %fp_b(i64 5)\n"
        "  %ind_sum = add i64 %res_a, %res_b\n"
        "  %s1 = call i64 @switch_eval(i32 1)\n"
        "  %s2 = call i64 @switch_eval(i32 2)\n"
        "  %sdef = call i64 @switch_eval(i32 99)\n"
        "  %sw_sum = add i64 %s1, %s2\n"
        "  %sw_total = add i64 %sw_sum, %sdef\n"
        "  %buf_src = alloca [4 x i8], align 4\n"
        "  %buf_dst = alloca [4 x i8], align 4\n"
        "  %buf_fill = alloca [4 x i8], align 4\n"
        "  %src0 = getelementptr inbounds [4 x i8], ptr %buf_src, i64 0, i64 0\n"
        "  %src1 = getelementptr inbounds [4 x i8], ptr %buf_src, i64 0, i64 1\n"
        "  %src2 = getelementptr inbounds [4 x i8], ptr %buf_src, i64 0, i64 2\n"
        "  %src3 = getelementptr inbounds [4 x i8], ptr %buf_src, i64 0, i64 3\n"
        "  store i8 1, ptr %src0, align 1\n"
        "  store i8 2, ptr %src1, align 1\n"
        "  store i8 3, ptr %src2, align 1\n"
        "  store i8 4, ptr %src3, align 1\n"
        "  call void @llvm.memset.p0.i64(ptr %buf_fill, i8 9, i64 4, i1 false)\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %buf_dst, ptr %buf_src, i64 4, i1 false)\n"
        "  %dst0 = getelementptr inbounds [4 x i8], ptr %buf_dst, i64 0, i64 0\n"
        "  %dst1 = getelementptr inbounds [4 x i8], ptr %buf_dst, i64 0, i64 1\n"
        "  call void @llvm.memmove.p0.p0.i64(ptr %dst1, ptr %dst0, i64 3, i1 false)\n"
        "  %dst2 = getelementptr inbounds [4 x i8], ptr %buf_dst, i64 0, i64 2\n"
        "  %dst3 = getelementptr inbounds [4 x i8], ptr %buf_dst, i64 0, i64 3\n"
        "  %fill0 = getelementptr inbounds [4 x i8], ptr %buf_fill, i64 0, i64 0\n"
        "  %v0 = load i8, ptr %dst0, align 1\n"
        "  %v1 = load i8, ptr %dst1, align 1\n"
        "  %v2 = load i8, ptr %dst2, align 1\n"
        "  %v3 = load i8, ptr %dst3, align 1\n"
        "  %vf = load i8, ptr %fill0, align 1\n"
        "  %x0 = zext i8 %v0 to i64\n"
        "  %x1 = zext i8 %v1 to i64\n"
        "  %x2 = zext i8 %v2 to i64\n"
        "  %x3 = zext i8 %v3 to i64\n"
        "  %xf = zext i8 %vf to i64\n"
        "  %w0 = mul i64 %x0, 1000\n"
        "  %w1 = mul i64 %x1, 100\n"
        "  %w2 = mul i64 %x2, 10\n"
        "  %m0 = add i64 %w0, %w1\n"
        "  %m1 = add i64 %w2, %x3\n"
        "  %memory = add i64 %m0, %m1\n"
        "  %memory_fill = add i64 %memory, %xf\n"
        "  %t1 = add i64 %ind_sum, %sw_total\n"
        "  %final = add i64 %t1, %memory_fill\n"
        "  ret i64 %final\n"
        "}\n";
    int p2a_translated = cvmir_translate(
        p2a_source, &options, &assembly, &error);
    if (!p2a_translated) {
        fprintf(stderr, "P2-A translation failed: %s\n", error.message);
    }
    assert(p2a_translated);
    assert(strstr(assembly, "CALLR R11") != NULL);
    assert(strstr(assembly, ".extern memset") != NULL);
    assert(strstr(assembly, ".extern memcpy") != NULL);
    assert(strstr(assembly, ".extern memmove") != NULL);
    assert(strstr(assembly, "CALLREL memset") != NULL);
    assert(strstr(assembly, "CALLREL memcpy") != NULL);
    assert(strstr(assembly, "CALLREL memmove") != NULL);
    CvmObjectFile p2a_object = {0};
    assert(assembler_assemble_object(assembly, &p2a_object,
                                     &assembly_error));
    unsigned runtime_symbols = 0;
    for (size_t i = 0; i < p2a_object.symbol_count; ++i) {
        const CvmObjectSymbol *symbol = &p2a_object.symbols[i];
        if (symbol->section_index != CVM_OBJECT_UNDEFINED_SECTION) continue;
        if (strcmp(symbol->name, "memcpy") == 0) runtime_symbols |= 1U;
        if (strcmp(symbol->name, "memset") == 0) runtime_symbols |= 2U;
        if (strcmp(symbol->name, "memmove") == 0) runtime_symbols |= 4U;
    }
    assert(runtime_symbols == 7U);
    cvm_object_destroy(&p2a_object);
    static const char p2a_entry[] =
        "MOVI64 SP, 8192\n"
        "CALL p2a_test\n"
        "HALT\n"
        "memcpy:\n"
        "    MOV R3, R0\n"
        ".p2a_memcpy_loop:\n"
        "    CMPI32 R2, 0\n"
        "    JZ .p2a_memcpy_done\n"
        "    LOAD8U R4, R1\n"
        "    STORE8 R0, R4\n"
        "    ADDI32 R0, 1\n"
        "    ADDI32 R1, 1\n"
        "    ADDI32 R2, -1\n"
        "    JUMP .p2a_memcpy_loop\n"
        ".p2a_memcpy_done:\n"
        "    MOV R0, R3\n"
        "    RET\n"
        "memset:\n"
        "    MOV R3, R0\n"
        ".p2a_memset_loop:\n"
        "    CMPI32 R2, 0\n"
        "    JZ .p2a_memset_done\n"
        "    STORE8 R0, R1\n"
        "    ADDI32 R0, 1\n"
        "    ADDI32 R2, -1\n"
        "    JUMP .p2a_memset_loop\n"
        ".p2a_memset_done:\n"
        "    MOV R0, R3\n"
        "    RET\n"
        "memmove:\n"
        "    MOV R3, R0\n"
        "    CMP R0, R1\n"
        "    JLTU .p2a_memmove_forward\n"
        "    JZ .p2a_memmove_done\n"
        "    ADD R0, R2\n"
        "    ADD R1, R2\n"
        ".p2a_memmove_backward_loop:\n"
        "    CMPI32 R2, 0\n"
        "    JZ .p2a_memmove_done\n"
        "    ADDI32 R0, -1\n"
        "    ADDI32 R1, -1\n"
        "    LOAD8U R4, R1\n"
        "    STORE8 R0, R4\n"
        "    ADDI32 R2, -1\n"
        "    JUMP .p2a_memmove_backward_loop\n"
        ".p2a_memmove_forward:\n"
        "    CMPI32 R2, 0\n"
        "    JZ .p2a_memmove_done\n"
        "    LOAD8U R4, R1\n"
        "    STORE8 R0, R4\n"
        "    ADDI32 R0, 1\n"
        "    ADDI32 R1, 1\n"
        "    ADDI32 R2, -1\n"
        "    JUMP .p2a_memmove_forward\n"
        ".p2a_memmove_done:\n"
        "    MOV R0, R3\n"
        "    RET\n";
    raw_source = ir_make_raw_source(assembly, p2a_entry);
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
    /* res_a = 15, res_b = 25 -> ind_sum = 40 */
    /* s1 = 100, s2 = 200, sdef = 300 -> sw_total = 600 */
    /* memmove makes dst={1,1,2,3}; memset contributes 9. */
    assert(cpu.registers[0] == 1772);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assembly_result_destroy(&binary);
    free(raw_source);
    free(assembly);
    return 0;
}
