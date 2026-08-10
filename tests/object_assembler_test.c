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
        ".entry entry\n"
        "entry: CALLREL helper\n"
        "HALT\n"
        ".section .rodata\n"
        "name: .asciz \"test\"\n"
        ".section .data\n"
        "value: .qword 42\n"
        ".section .bss\n"
        "scratch: .space 4096\n";
    CvmObjectFile object;
    AssemblyError error;
    assert(assembler_assemble_object(source, &object, &error));
    assert(object.section_count == 4);
    assert((object.sections[0].flags & CVM_OBJECT_SECTION_EXECUTE) != 0);
    assert((object.sections[3].flags & CVM_OBJECT_SECTION_NOBITS) != 0);
    const CvmObjectSymbol *entry = find_symbol(&object, "entry");
    const CvmObjectSymbol *helper = find_symbol(&object, "helper");
    assert(entry != NULL &&
           (entry->flags & (CVM_OBJECT_SYMBOL_GLOBAL |
                            CVM_OBJECT_SYMBOL_DEFINED |
                            CVM_OBJECT_SYMBOL_ENTRY)) != 0);
    assert(helper != NULL &&
           (helper->flags & CVM_OBJECT_SYMBOL_DEFINED) == 0);

    static const char path[] = "build/object_roundtrip.o";
    char format_error[160];
    assert(cvm_object_write(path, &object, format_error,
                            sizeof(format_error)));
    CvmObjectFile decoded;
    assert(cvm_object_read(path, &decoded, format_error,
                           sizeof(format_error)));
    assert(decoded.section_count == object.section_count);
    assert(decoded.symbol_count == object.symbol_count);
    assert(strcmp(decoded.sections[1].name, ".rodata") == 0);
    cvm_object_destroy(&decoded);
    cvm_object_destroy(&object);
    assert(remove(path) == 0);
    return 0;
}
