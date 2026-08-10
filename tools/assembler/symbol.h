#ifndef VM_ASSEMBLER_SYMBOL_H
#define VM_ASSEMBLER_SYMBOL_H

#include "assembler.h"

typedef struct {
    AssemblerSymbol *items;
    size_t count;
    size_t capacity;
} AsmSymbolTable;

void asm_symbol_table_init(AsmSymbolTable *table);
void asm_symbol_table_destroy(AsmSymbolTable *table);
int asm_symbol_define(AsmSymbolTable *table,
                      const char *name,
                      uint64_t address);
int asm_symbol_lookup(const AsmSymbolTable *table,
                      const char *name,
                      uint64_t *address);

#endif
