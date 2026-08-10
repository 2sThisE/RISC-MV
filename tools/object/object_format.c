#include "object_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBJECT_HEADER_SIZE ((size_t)80)
#define OBJECT_SECTION_SIZE ((size_t)48)
#define OBJECT_SYMBOL_SIZE ((size_t)32)
#define OBJECT_RELOCATION_SIZE ((size_t)32)

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0)
        (void)snprintf(error, size, "%s", message);
}

static void write_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *out, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        out[i] = (uint8_t)(value >> (i * 8));
}

static void write_u64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        out[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t read_u16(const uint8_t *in)
{
    return (uint16_t)((uint16_t)in[0] | (uint16_t)in[1] << 8);
}

static uint32_t read_u32(const uint8_t *in)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value |= (uint32_t)in[i] << (i * 8);
    return value;
}

static uint64_t read_u64(const uint8_t *in)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= (uint64_t)in[i] << (i * 8);
    return value;
}

static int add_size(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) return 0;
    *result = left + right;
    return 1;
}

static int multiply_size(size_t left, size_t right, size_t *result)
{
    if (left != 0 && right > SIZE_MAX / left) return 0;
    *result = left * right;
    return 1;
}

static int range_valid(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= (uint64_t)size - offset;
}

static int power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static char *copy_string(const char *text, size_t size)
{
    if (size == SIZE_MAX) return NULL;
    char *copy = malloc(size + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, size);
    copy[size] = '\0';
    return copy;
}

size_t cvm_object_relocation_width(uint16_t type)
{
    switch (type) {
    case CVM_OBJECT_RELOCATION_ABS32:
    case CVM_OBJECT_RELOCATION_REL32:
        return 4;
    case CVM_OBJECT_RELOCATION_ABS64:
        return 8;
    default:
        return 0;
    }
}

void cvm_object_destroy(CvmObjectFile *object)
{
    if (object == NULL) return;
    for (size_t i = 0; i < object->section_count; ++i) {
        free(object->sections[i].name);
        free(object->sections[i].data);
    }
    for (size_t i = 0; i < object->symbol_count; ++i)
        free(object->symbols[i].name);
    free(object->sections);
    free(object->symbols);
    free(object->relocations);
    *object = (CvmObjectFile){0};
}

static int object_valid(const CvmObjectFile *object)
{
    if (object == NULL || object->section_count == 0 ||
        object->section_count > CVM_OBJECT_MAX_SECTIONS ||
        object->symbol_count > CVM_OBJECT_MAX_SYMBOLS ||
        object->relocation_count > CVM_OBJECT_MAX_RELOCATIONS ||
        object->sections == NULL ||
        (object->symbol_count != 0 && object->symbols == NULL) ||
        (object->relocation_count != 0 && object->relocations == NULL))
        return 0;
    for (size_t i = 0; i < object->section_count; ++i) {
        const CvmObjectSection *section = &object->sections[i];
        uint32_t allowed = CVM_OBJECT_SECTION_ALLOC |
                           CVM_OBJECT_SECTION_WRITE |
                           CVM_OBJECT_SECTION_EXECUTE |
                           CVM_OBJECT_SECTION_NOBITS;
        int nobits = (section->flags & CVM_OBJECT_SECTION_NOBITS) != 0;
        if (section->name == NULL || section->name[0] == '\0' ||
            !power_of_two(section->alignment) ||
            (section->flags & ~allowed) != 0 ||
            (section->flags & CVM_OBJECT_SECTION_ALLOC) == 0 ||
            ((section->flags & CVM_OBJECT_SECTION_WRITE) != 0 &&
             (section->flags & CVM_OBJECT_SECTION_EXECUTE) != 0) ||
            section->file_size > section->memory_size ||
            (nobits && section->file_size != 0) ||
            (!nobits && section->file_size != 0 && section->data == NULL))
            return 0;
    }
    for (size_t i = 0; i < object->symbol_count; ++i) {
        const CvmObjectSymbol *symbol = &object->symbols[i];
        uint16_t allowed = CVM_OBJECT_SYMBOL_GLOBAL |
                           CVM_OBJECT_SYMBOL_DEFINED |
                           CVM_OBJECT_SYMBOL_ENTRY |
                           CVM_OBJECT_SYMBOL_WEAK |
                           CVM_OBJECT_SYMBOL_FUNCTION |
                           CVM_OBJECT_SYMBOL_OBJECT |
                           CVM_OBJECT_SYMBOL_COMMON;
        int defined = (symbol->flags & CVM_OBJECT_SYMBOL_DEFINED) != 0;
        int common = (symbol->flags & CVM_OBJECT_SYMBOL_COMMON) != 0;
        if (symbol->name == NULL || symbol->name[0] == '\0' ||
            (symbol->flags & (uint16_t)~allowed) != 0 ||
            ((symbol->flags & CVM_OBJECT_SYMBOL_FUNCTION) != 0 &&
             (symbol->flags & CVM_OBJECT_SYMBOL_OBJECT) != 0) ||
            ((symbol->flags & CVM_OBJECT_SYMBOL_WEAK) != 0 &&
             (symbol->flags & CVM_OBJECT_SYMBOL_GLOBAL) == 0) ||
            (common && (defined ||
                        (symbol->flags & CVM_OBJECT_SYMBOL_GLOBAL) == 0 ||
                        symbol->section_index != CVM_OBJECT_COMMON_SECTION ||
                        symbol->size == 0 || !power_of_two(symbol->value))) ||
            (defined &&
             symbol->section_index != CVM_OBJECT_ABSOLUTE_SECTION &&
             symbol->section_index >= object->section_count) ||
            (!defined && !common &&
             symbol->section_index != CVM_OBJECT_UNDEFINED_SECTION) ||
            ((symbol->flags & CVM_OBJECT_SYMBOL_ENTRY) != 0 &&
             (!defined || common)) ||
            (defined &&
             symbol->section_index != CVM_OBJECT_ABSOLUTE_SECTION &&
             (symbol->value >
                  object->sections[symbol->section_index].memory_size ||
              symbol->size >
                  object->sections[symbol->section_index].memory_size -
                      symbol->value)))
            return 0;
    }
    for (size_t i = 0; i < object->relocation_count; ++i) {
        const CvmObjectRelocation *relocation = &object->relocations[i];
        size_t width = cvm_object_relocation_width(relocation->type);
        if (relocation->section_index >= object->section_count || width == 0 ||
            relocation->symbol_index >= object->symbol_count ||
            relocation->offset >
                object->sections[relocation->section_index].file_size ||
            width > object->sections[relocation->section_index].file_size -
                        (size_t)relocation->offset)
            return 0;
    }
    return 1;
}

int cvm_object_write(const char *path, const CvmObjectFile *object,
                     char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (path == NULL || !object_valid(object)) {
        set_error(error, error_size, "invalid object data");
        return 0;
    }
    size_t section_bytes, symbol_bytes, relocation_bytes, cursor;
    if (!multiply_size(object->section_count, OBJECT_SECTION_SIZE,
                       &section_bytes) ||
        !multiply_size(object->symbol_count, OBJECT_SYMBOL_SIZE,
                       &symbol_bytes) ||
        !multiply_size(object->relocation_count, OBJECT_RELOCATION_SIZE,
                       &relocation_bytes) ||
        !add_size(OBJECT_HEADER_SIZE, section_bytes, &cursor) ||
        !add_size(cursor, symbol_bytes, &cursor) ||
        !add_size(cursor, relocation_bytes, &cursor)) {
        set_error(error, error_size, "object table size overflow");
        return 0;
    }
    size_t string_offset = cursor;
    for (size_t i = 0; i < object->section_count; ++i) {
        if (!add_size(cursor, strlen(object->sections[i].name) + 1, &cursor)) {
            set_error(error, error_size, "object string table overflow");
            return 0;
        }
    }
    for (size_t i = 0; i < object->symbol_count; ++i) {
        if (!add_size(cursor, strlen(object->symbols[i].name) + 1, &cursor)) {
            set_error(error, error_size, "object string table overflow");
            return 0;
        }
    }
    size_t string_size = cursor - string_offset;
    for (size_t i = 0; i < object->section_count; ++i) {
        if (!add_size(cursor, object->sections[i].file_size, &cursor)) {
            set_error(error, error_size, "object section data overflow");
            return 0;
        }
    }
    if (cursor > CVM_OBJECT_MAX_SIZE) {
        set_error(error, error_size, "object exceeds maximum size");
        return 0;
    }
    uint8_t *data = calloc(cursor, 1);
    if (data == NULL) {
        set_error(error, error_size, "cannot allocate object image");
        return 0;
    }
    memcpy(data, CVM_OBJECT_MAGIC, 8);
    write_u32(data + 0x08, CVM_OBJECT_VERSION);
    write_u32(data + 0x0C, (uint32_t)object->section_count);
    write_u32(data + 0x10, (uint32_t)object->symbol_count);
    write_u32(data + 0x14, (uint32_t)object->relocation_count);
    write_u64(data + 0x18, OBJECT_HEADER_SIZE);
    write_u64(data + 0x20, OBJECT_HEADER_SIZE + section_bytes);
    write_u64(data + 0x28,
              OBJECT_HEADER_SIZE + section_bytes + symbol_bytes);
    write_u64(data + 0x30, string_offset);
    write_u64(data + 0x38, string_size);
    write_u64(data + 0x40, cursor);

    size_t string_cursor = string_offset;
    size_t file_cursor = string_offset + string_size;
    for (size_t i = 0; i < object->section_count; ++i) {
        const CvmObjectSection *section = &object->sections[i];
        uint8_t *entry = data + OBJECT_HEADER_SIZE + i * OBJECT_SECTION_SIZE;
        write_u32(entry + 0x00, (uint32_t)(string_cursor - string_offset));
        write_u32(entry + 0x04, section->flags);
        write_u64(entry + 0x08, section->alignment);
        write_u64(entry + 0x10, file_cursor);
        write_u64(entry + 0x18, section->file_size);
        write_u64(entry + 0x20, section->memory_size);
        size_t length = strlen(section->name) + 1;
        memcpy(data + string_cursor, section->name, length);
        string_cursor += length;
        if (section->file_size != 0) {
            memcpy(data + file_cursor, section->data, section->file_size);
            file_cursor += section->file_size;
        }
    }
    for (size_t i = 0; i < object->symbol_count; ++i) {
        const CvmObjectSymbol *symbol = &object->symbols[i];
        uint8_t *entry = data + OBJECT_HEADER_SIZE + section_bytes +
                         i * OBJECT_SYMBOL_SIZE;
        write_u32(entry + 0x00, (uint32_t)(string_cursor - string_offset));
        write_u16(entry + 0x04, symbol->section_index);
        write_u16(entry + 0x06, symbol->flags);
        write_u64(entry + 0x08, symbol->value);
        write_u64(entry + 0x10, symbol->size);
        size_t length = strlen(symbol->name) + 1;
        memcpy(data + string_cursor, symbol->name, length);
        string_cursor += length;
    }
    for (size_t i = 0; i < object->relocation_count; ++i) {
        const CvmObjectRelocation *relocation = &object->relocations[i];
        uint8_t *entry = data + OBJECT_HEADER_SIZE + section_bytes +
                         symbol_bytes + i * OBJECT_RELOCATION_SIZE;
        write_u16(entry + 0x00, relocation->section_index);
        write_u16(entry + 0x02, relocation->type);
        write_u32(entry + 0x04, relocation->symbol_index);
        write_u64(entry + 0x08, relocation->offset);
        write_u64(entry + 0x10, (uint64_t)relocation->addend);
    }

    FILE *file = fopen(path, "wb");
    int okay = file != NULL && fwrite(data, 1, cursor, file) == cursor;
    if (file != NULL && fclose(file) != 0) okay = 0;
    free(data);
    if (!okay) set_error(error, error_size, "cannot write object file");
    return okay;
}

static int read_name(const uint8_t *data, size_t file_size,
                     size_t string_offset, size_t string_size,
                     uint32_t relative, char **result)
{
    if (relative >= string_size) return 0;
    size_t start = string_offset + relative;
    size_t maximum = string_size - relative;
    const uint8_t *end = memchr(data + start, 0, maximum);
    if (end == NULL) return 0;
    size_t length = (size_t)(end - (data + start));
    if (length == 0 || start + length >= file_size) return 0;
    *result = copy_string((const char *)data + start, length);
    return *result != NULL;
}

int cvm_object_decode(const void *contents, size_t size,
                      CvmObjectFile *object, char *error, size_t error_size)
{
    if (object == NULL) return 0;
    *object = (CvmObjectFile){0};
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (contents == NULL || size < OBJECT_HEADER_SIZE ||
        size > CVM_OBJECT_MAX_SIZE) {
        set_error(error, error_size, "invalid object data size");
        return 0;
    }
    uint8_t *data = malloc(size);
    if (data == NULL) {
        free(data);
        set_error(error, error_size, "cannot allocate object decoder");
        return 0;
    }
    memcpy(data, contents, size);
    uint32_t section_count = read_u32(data + 0x0C);
    uint32_t symbol_count = read_u32(data + 0x10);
    uint32_t relocation_count = read_u32(data + 0x14);
    uint64_t section_offset = read_u64(data + 0x18);
    uint64_t symbol_offset = read_u64(data + 0x20);
    uint64_t relocation_offset = read_u64(data + 0x28);
    uint64_t string_offset64 = read_u64(data + 0x30);
    uint64_t string_size64 = read_u64(data + 0x38);
    uint64_t encoded_size = read_u64(data + 0x40);
    size_t section_bytes, symbol_bytes, relocation_bytes;
    if (memcmp(data, CVM_OBJECT_MAGIC, 8) != 0 ||
        read_u32(data + 0x08) != CVM_OBJECT_VERSION || encoded_size != size ||
        section_count == 0 || section_count > CVM_OBJECT_MAX_SECTIONS ||
        symbol_count > CVM_OBJECT_MAX_SYMBOLS ||
        relocation_count > CVM_OBJECT_MAX_RELOCATIONS ||
        !multiply_size(section_count, OBJECT_SECTION_SIZE, &section_bytes) ||
        !multiply_size(symbol_count, OBJECT_SYMBOL_SIZE, &symbol_bytes) ||
        !multiply_size(relocation_count, OBJECT_RELOCATION_SIZE,
                       &relocation_bytes) ||
        !range_valid(size, section_offset, section_bytes) ||
        !range_valid(size, symbol_offset, symbol_bytes) ||
        !range_valid(size, relocation_offset, relocation_bytes) ||
        !range_valid(size, string_offset64, string_size64) ||
        string_offset64 > SIZE_MAX || string_size64 > SIZE_MAX) {
        free(data);
        set_error(error, error_size, "invalid object header or tables");
        return 0;
    }
    size_t string_offset = (size_t)string_offset64;
    size_t string_size = (size_t)string_size64;
    object->sections = calloc(section_count, sizeof(*object->sections));
    object->symbols = calloc(symbol_count, sizeof(*object->symbols));
    object->relocations = calloc(relocation_count,
                                 sizeof(*object->relocations));
    if (object->sections == NULL ||
        (symbol_count != 0 && object->symbols == NULL) ||
        (relocation_count != 0 && object->relocations == NULL)) {
        free(data);
        cvm_object_destroy(object);
        set_error(error, error_size, "cannot allocate decoded object");
        return 0;
    }
    object->section_count = section_count;
    object->symbol_count = symbol_count;
    object->relocation_count = relocation_count;
    for (size_t i = 0; i < section_count; ++i) {
        const uint8_t *entry = data + (size_t)section_offset +
                               i * OBJECT_SECTION_SIZE;
        CvmObjectSection *section = &object->sections[i];
        section->flags = read_u32(entry + 0x04);
        section->alignment = read_u64(entry + 0x08);
        uint64_t data_offset = read_u64(entry + 0x10);
        uint64_t file_size64 = read_u64(entry + 0x18);
        section->memory_size = read_u64(entry + 0x20);
        if (!read_name(data, size, string_offset, string_size,
                       read_u32(entry + 0x00), &section->name) ||
            !range_valid(size, data_offset, file_size64) ||
            file_size64 > SIZE_MAX) {
            free(data);
            cvm_object_destroy(object);
            set_error(error, error_size, "invalid object section");
            return 0;
        }
        section->file_size = (size_t)file_size64;
        if (section->file_size != 0) {
            section->data = malloc(section->file_size);
            if (section->data == NULL) {
                free(data);
                cvm_object_destroy(object);
                set_error(error, error_size, "cannot allocate section data");
                return 0;
            }
            memcpy(section->data, data + (size_t)data_offset,
                   section->file_size);
        }
    }
    for (size_t i = 0; i < symbol_count; ++i) {
        const uint8_t *entry = data + (size_t)symbol_offset +
                               i * OBJECT_SYMBOL_SIZE;
        CvmObjectSymbol *symbol = &object->symbols[i];
        symbol->section_index = read_u16(entry + 0x04);
        symbol->flags = read_u16(entry + 0x06);
        symbol->value = read_u64(entry + 0x08);
        symbol->size = read_u64(entry + 0x10);
        if (!read_name(data, size, string_offset, string_size,
                       read_u32(entry + 0x00), &symbol->name)) {
            free(data);
            cvm_object_destroy(object);
            set_error(error, error_size, "invalid object symbol");
            return 0;
        }
    }
    for (size_t i = 0; i < relocation_count; ++i) {
        const uint8_t *entry = data + (size_t)relocation_offset +
                               i * OBJECT_RELOCATION_SIZE;
        object->relocations[i] = (CvmObjectRelocation){
            .section_index = read_u16(entry + 0x00),
            .type = read_u16(entry + 0x02),
            .symbol_index = read_u32(entry + 0x04),
            .offset = read_u64(entry + 0x08),
            .addend = (int64_t)read_u64(entry + 0x10)
        };
    }
    free(data);
    if (!object_valid(object)) {
        cvm_object_destroy(object);
        set_error(error, error_size, "object contents violate format rules");
        return 0;
    }
    return 1;
}

int cvm_object_read(const char *path, CvmObjectFile *object,
                    char *error, size_t error_size)
{
    if (object == NULL) return 0;
    *object = (CvmObjectFile){0};
    if (error != NULL && error_size != 0) error[0] = '\0';
    FILE *file = path != NULL ? fopen(path, "rb") : NULL;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        set_error(error, error_size, "cannot open object file");
        return 0;
    }
    long measured = ftell(file);
    if (measured < (long)OBJECT_HEADER_SIZE ||
        (unsigned long)measured > CVM_OBJECT_MAX_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(error, error_size, "invalid object file size");
        return 0;
    }
    size_t size = (size_t)measured;
    uint8_t *data = malloc(size);
    if (data == NULL || fread(data, 1, size, file) != size ||
        fclose(file) != 0) {
        free(data);
        set_error(error, error_size, "cannot read object file");
        return 0;
    }
    int okay = cvm_object_decode(data, size, object, error, error_size);
    free(data);
    return okay;
}
