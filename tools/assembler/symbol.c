#include "symbol.h"

#include <stdlib.h>
#include <string.h>

static char *asm_duplicate_string(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

void asm_symbol_table_init(AsmSymbolTable *table)
{
    if (table != NULL) {
        *table = (AsmSymbolTable){0};
    }
}

void asm_symbol_table_destroy(AsmSymbolTable *table)
{
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->count; ++i) {
        free(table->items[i].name);
    }
    free(table->items);
    *table = (AsmSymbolTable){0};
}

int asm_symbol_lookup(const AsmSymbolTable *table,
                      const char *name,
                      uint64_t *address)
{
    if (table == NULL || name == NULL) {
        return 0;
    }
    for (size_t i = 0; i < table->count; ++i) {
        if (strcmp(table->items[i].name, name) == 0) {
            if (address != NULL) {
                *address = table->items[i].address;
            }
            return 1;
        }
    }
    return 0;
}

int asm_symbol_define(AsmSymbolTable *table,
                      const char *name,
                      uint64_t address)
{
    if (table == NULL || name == NULL || *name == '\0' ||
        asm_symbol_lookup(table, name, NULL)) {
        return 0;
    }
    if (table->count == table->capacity) {
        size_t next_capacity = table->capacity == 0
                                   ? 32
                                   : table->capacity * 2;
        if (next_capacity < table->capacity ||
            next_capacity > SIZE_MAX / sizeof(*table->items)) {
            return 0;
        }
        AssemblerSymbol *replacement = realloc(
            table->items,
            next_capacity * sizeof(*table->items));
        if (replacement == NULL) {
            return 0;
        }
        table->items = replacement;
        table->capacity = next_capacity;
    }

    char *copy = asm_duplicate_string(name);
    if (copy == NULL) {
        return 0;
    }
    table->items[table->count++] = (AssemblerSymbol){
        .name = copy,
        .address = address
    };
    return 1;
}
