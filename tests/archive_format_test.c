#include "archive_format.h"
#include "object_assembler.h"
#include "object_format.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *duplicate_text(const char *text)
{
    size_t size = strlen(text);
    char *copy = malloc(size + 1);
    assert(copy != NULL);
    memcpy(copy, text, size + 1);
    return copy;
}

static uint8_t *read_bytes(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long measured = ftell(file);
    assert(measured > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    *size = (size_t)measured;
    uint8_t *data = malloc(*size);
    assert(data != NULL);
    assert(fread(data, 1, *size, file) == *size);
    assert(fclose(file) == 0);
    return data;
}

int test_archive_format(void)
{
    static const char source[] =
        ".section .text\n"
        ".global helper\n"
        "helper: MOVI32U R0, 42\n"
        "RET\n";
    CvmObjectFile object;
    AssemblyError assembly_error;
    assert(assembler_assemble_object(source, &object, &assembly_error));
    static const char object_path[] = "build/archive_member.o";
    static const char archive_path[] = "build/archive_roundtrip.a";
    char error[160];
    assert(cvm_object_write(object_path, &object, error, sizeof(error)));
    cvm_object_destroy(&object);

    CvmArchive archive = {0};
    archive.member_count = 1;
    archive.members = calloc(1, sizeof(*archive.members));
    archive.symbol_count = 1;
    archive.symbols = calloc(1, sizeof(*archive.symbols));
    assert(archive.members != NULL && archive.symbols != NULL);
    archive.members[0].name = duplicate_text("archive_member.o");
    archive.members[0].data = read_bytes(object_path,
                                         &archive.members[0].size);
    archive.symbols[0].name = duplicate_text("helper");
    archive.symbols[0].member_index = 0;
    assert(cvm_archive_write(archive_path, &archive, error, sizeof(error)));
    cvm_archive_destroy(&archive);

    CvmArchive decoded;
    assert(cvm_archive_read(archive_path, &decoded, error, sizeof(error)));
    assert(decoded.member_count == 1);
    assert(decoded.symbol_count == 1);
    assert(strcmp(decoded.members[0].name, "archive_member.o") == 0);
    assert(strcmp(decoded.symbols[0].name, "helper") == 0);
    assert(cvm_object_decode(decoded.members[0].data,
                             decoded.members[0].size,
                             &object, error, sizeof(error)));
    assert(object.symbol_count == 1);
    assert(strcmp(object.symbols[0].name, "helper") == 0);
    cvm_object_destroy(&object);
    cvm_archive_destroy(&decoded);
    assert(remove(object_path) == 0);
    assert(remove(archive_path) == 0);
    return 0;
}
