#include "object_assembler.h"
#include "object_format.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const CvmObjectSymbol *find_symbol(const CvmObjectFile *object,
                                          const char *name)
{
    for (size_t i = 0; i < object->symbol_count; ++i) {
        if (strcmp(object->symbols[i].name, name) == 0)
            return &object->symbols[i];
    }
    return NULL;
}

int test_object_assembler(void)
{
    static const char source[] =
        ".section .text\n"
        ".global entry\n"
        ".extern helper\n"
        ".weak optional_hook\n"
        ".extern optional_hook\n"
        ".type entry, function\n"
        ".type helper, function\n"
        ".type optional_hook, function\n"
        ".entry entry\n"
        "entry: CALLREL helper\n"
        "MOVI64 R3, optional_hook\n"
        "HALT\n"
        ".size entry, $ - entry\n"
        ".section .rodata\n"
        "name: .asciz \"test\"\n"
        ".section .data\n"
        "value: .qword 42\n"
        "entry_pointer: .qword entry\n"
        ".section .bss\n"
        "scratch: .zero 4096\n"
        ".comm shared_buffer, 32, 16\n";
    CvmObjectFile object;
    AssemblyError error;
    assert(assembler_assemble_object(source, &object, &error));
    assert(object.section_count == 4);
    assert((object.sections[0].flags & CVM_OBJECT_SECTION_EXECUTE) != 0);
    assert((object.sections[3].flags & CVM_OBJECT_SECTION_NOBITS) != 0);
    assert(object.sections[0].file_size == 16);
    assert(object.sections[3].file_size == 0);
    assert(object.sections[3].memory_size == 4096);
    const CvmObjectSymbol *entry = find_symbol(&object, "entry");
    const CvmObjectSymbol *helper = find_symbol(&object, "helper");
    const CvmObjectSymbol *optional = find_symbol(&object, "optional_hook");
    const CvmObjectSymbol *common = find_symbol(&object, "shared_buffer");
    assert(entry != NULL &&
           (entry->flags & (CVM_OBJECT_SYMBOL_GLOBAL |
                            CVM_OBJECT_SYMBOL_DEFINED |
                            CVM_OBJECT_SYMBOL_ENTRY)) != 0);
    assert((entry->flags & CVM_OBJECT_SYMBOL_FUNCTION) != 0);
    assert(entry->size == 16);
    assert(helper != NULL &&
           (helper->flags & CVM_OBJECT_SYMBOL_DEFINED) == 0);
    assert((helper->flags & CVM_OBJECT_SYMBOL_FUNCTION) != 0);
    assert(optional != NULL &&
           (optional->flags & CVM_OBJECT_SYMBOL_WEAK) != 0 &&
           (optional->flags & CVM_OBJECT_SYMBOL_DEFINED) == 0);
    assert(common != NULL &&
           (common->flags & (CVM_OBJECT_SYMBOL_GLOBAL |
                             CVM_OBJECT_SYMBOL_COMMON |
                             CVM_OBJECT_SYMBOL_OBJECT)) != 0);
    assert(common->section_index == CVM_OBJECT_COMMON_SECTION);
    assert(common->size == 32 && common->value == 16);
    assert(object.relocation_count == 3);
    assert(object.relocations[0].type == CVM_OBJECT_RELOCATION_REL32);
    assert(object.relocations[0].offset == 1);
    assert(strcmp(object.symbols[object.relocations[0].symbol_index].name,
                  "helper") == 0);
    assert(object.relocations[1].type == CVM_OBJECT_RELOCATION_ABS64);
    assert(object.relocations[1].section_index == 0);
    assert(object.relocations[1].offset == 7);
    assert(strcmp(object.symbols[object.relocations[1].symbol_index].name,
                  "optional_hook") == 0);
    assert(object.relocations[2].type == CVM_OBJECT_RELOCATION_ABS64);
    assert(object.relocations[2].section_index == 2);
    assert(object.relocations[2].offset == 8);
    assert(strcmp(object.symbols[object.relocations[2].symbol_index].name,
                  "entry") == 0);

    static const char path[] = "build/object_roundtrip.o";
    char format_error[160];
    assert(cvm_object_write(path, &object, format_error,
                            sizeof(format_error)));
    CvmObjectFile decoded;
    assert(cvm_object_read(path, &decoded, format_error,
                           sizeof(format_error)));
    assert(decoded.section_count == object.section_count);
    assert(decoded.symbol_count == object.symbol_count);
    assert(decoded.relocation_count == object.relocation_count);
    assert(strcmp(decoded.sections[1].name, ".rodata") == 0);
    assert(decoded.sections[0].file_size == object.sections[0].file_size);
    assert(memcmp(decoded.sections[0].data, object.sections[0].data,
                  object.sections[0].file_size) == 0);
    cvm_object_destroy(&decoded);
    cvm_object_destroy(&object);
    assert(remove(path) == 0);
    return 0;
}
