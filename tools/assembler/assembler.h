#ifndef VM_ASSEMBLER_H
#define VM_ASSEMBLER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint64_t address;
} AssemblerSymbol;

typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t base_address;
    uint64_t entry_address;
    int entry_set;
    AssemblerSymbol *symbols;
    size_t symbol_count;
} AssemblyResult;

typedef struct {
    size_t line;
    size_t column;
    char message[256];
} AssemblyError;

int assembler_assemble(const char *source,
                       uint64_t base_address,
                       AssemblyResult *result,
                       AssemblyError *error);
void assembly_result_destroy(AssemblyResult *result);

#endif
