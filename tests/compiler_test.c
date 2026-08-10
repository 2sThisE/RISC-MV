#include "assembler.h"
#include "bus.h"
#include "compiler.h"
#include "cpu.h"
#include "object_assembler.h"
#include "object_format.h"
#include "ram.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const CvmObjectSymbol *compiler_find_symbol(
    const CvmObjectFile *object, const char *name)
{
    for (size_t i = 0; i < object->symbol_count; ++i) {
        if (strcmp(object->symbols[i].name, name) == 0) {
            return &object->symbols[i];
        }
    }
    return NULL;
}

static char *compiler_make_raw_source(const char *assembly)
{
    static const char prefix[] =
        "MOVI64 SP, 4096\n"
        "CALL run\n"
        "HALT\n"
        "result: .qword 0\n";
    size_t capacity = sizeof(prefix) + strlen(assembly) + 1;
    char *raw = malloc(capacity);
    assert(raw != NULL);
    strcpy(raw, prefix);
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
                       strncmp(line, ".comm ", 6) == 0;
        if (!metadata) {
            memcpy(raw + length, line, line_length);
            length += line_length;
        }
        line += line_length;
    }
    raw[length] = '\0';
    return raw;
}

int test_compiler(void)
{
    static const char source[] =
        "long result;\n"
        "long add(long a, long b, long c) { return a + b + c; }\n"
        "int run(void) {\n"
        "  long n = 3; long sum = 0;\n"
        "  while (n > 0) { sum = sum + n; n = n - 1; }\n"
        "  if (sum == 6 && add(1, 2, 3) == 6) { result = sum; return 0; }\n"
        "  return 1;\n"
        "}\n";
    char *assembly = NULL;
    CvmCompilerError compiler_error;
    assert(cvm_compile_c_source(source, &assembly, &compiler_error));
    assert(assembly != NULL);
    assert(strstr(assembly, ".global add") != NULL);
    assert(strstr(assembly, "STORE64O R14, R2, -24") != NULL);
    assert(strstr(assembly, "CALLREL add") != NULL);
    assert(strstr(assembly, ".comm result, 8, 8") != NULL);

    CvmObjectFile object;
    AssemblyError assembler_error;
    assert(assembler_assemble_object(assembly, &object, &assembler_error));
    const CvmObjectSymbol *add = compiler_find_symbol(&object, "add");
    const CvmObjectSymbol *run = compiler_find_symbol(&object, "run");
    const CvmObjectSymbol *result = compiler_find_symbol(&object, "result");
    assert(add != NULL &&
           (add->flags & CVM_OBJECT_SYMBOL_FUNCTION) != 0);
    assert(run != NULL &&
           (run->flags & CVM_OBJECT_SYMBOL_FUNCTION) != 0);
    assert(result != NULL &&
           (result->flags & CVM_OBJECT_SYMBOL_COMMON) != 0);
    assert(object.relocation_count != 0);
    cvm_object_destroy(&object);

    char *raw_source = compiler_make_raw_source(assembly);
    AssemblyResult binary;
    assert(assembler_assemble(raw_source, 1, &binary, &assembler_error));
    assert(binary.size < 4095);
    uint8_t memory[4096] = {0};
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
    assert(cpu.registers[0] == 0);
    assert(cpu.registers[REGISTER_SP] == sizeof(memory));
    assembly_result_destroy(&binary);
    free(raw_source);
    free(assembly);

    static const char invalid[] = "int bad(void) { return missing; }";
    assembly = NULL;
    assert(!cvm_compile_c_source(invalid, &assembly, &compiler_error));
    assert(compiler_error.line == 1);
    assert(strstr(compiler_error.message, "unknown variable") != NULL);
    return 0;
}
