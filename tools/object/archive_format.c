#include "archive_format.h"

#include "object_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_HEADER_SIZE ((size_t)64)
#define ARCHIVE_MEMBER_SIZE ((size_t)32)
#define ARCHIVE_SYMBOL_SIZE ((size_t)16)

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0)
        (void)snprintf(error, size, "%s", message);
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

static int align_size(size_t value, size_t alignment, size_t *result)
{
    size_t mask = alignment - 1;
    if (value > SIZE_MAX - mask) return 0;
    *result = (value + mask) & ~mask;
    return 1;
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

static int archive_valid(const CvmArchive *archive)
{
    if (archive == NULL || archive->member_count == 0 ||
        archive->member_count > CVM_ARCHIVE_MAX_MEMBERS ||
        archive->symbol_count > CVM_ARCHIVE_MAX_SYMBOLS ||
        archive->members == NULL ||
        (archive->symbol_count != 0 && archive->symbols == NULL)) return 0;
    for (size_t i = 0; i < archive->member_count; ++i) {
        if (archive->members[i].name == NULL ||
            archive->members[i].name[0] == '\0' ||
            archive->members[i].data == NULL ||
            archive->members[i].size == 0 ||
            archive->members[i].size > CVM_OBJECT_MAX_SIZE) return 0;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(archive->members[i].name,
                       archive->members[j].name) == 0) return 0;
    }
    for (size_t i = 0; i < archive->symbol_count; ++i) {
        if (archive->symbols[i].name == NULL ||
            archive->symbols[i].name[0] == '\0' ||
            archive->symbols[i].member_index >= archive->member_count ||
            (archive->symbols[i].flags & ~CVM_ARCHIVE_SYMBOL_ENTRY) != 0)
            return 0;
    }
    return 1;
}

void cvm_archive_destroy(CvmArchive *archive)
{
    if (archive == NULL) return;
    for (size_t i = 0; i < archive->member_count; ++i) {
        free(archive->members[i].name);
        free(archive->members[i].data);
    }
    for (size_t i = 0; i < archive->symbol_count; ++i)
        free(archive->symbols[i].name);
    free(archive->members);
    free(archive->symbols);
    *archive = (CvmArchive){0};
}

int cvm_archive_write(const char *path, const CvmArchive *archive,
                      char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (path == NULL || !archive_valid(archive)) {
        set_error(error, error_size, "invalid archive data");
        return 0;
    }
    size_t member_bytes, symbol_bytes, cursor;
    if (!multiply_size(archive->member_count, ARCHIVE_MEMBER_SIZE,
                       &member_bytes) ||
        !multiply_size(archive->symbol_count, ARCHIVE_SYMBOL_SIZE,
                       &symbol_bytes) ||
        !add_size(ARCHIVE_HEADER_SIZE, member_bytes, &cursor) ||
        !add_size(cursor, symbol_bytes, &cursor)) {
        set_error(error, error_size, "archive table size overflow");
        return 0;
    }
    size_t string_offset = cursor;
    for (size_t i = 0; i < archive->member_count; ++i)
        if (!add_size(cursor, strlen(archive->members[i].name) + 1, &cursor)) {
            set_error(error, error_size, "archive string table overflow");
            return 0;
        }
    for (size_t i = 0; i < archive->symbol_count; ++i)
        if (!add_size(cursor, strlen(archive->symbols[i].name) + 1, &cursor)) {
            set_error(error, error_size, "archive string table overflow");
            return 0;
        }
    size_t string_size = cursor - string_offset;
    if (!align_size(cursor, 8, &cursor)) {
        set_error(error, error_size, "archive data alignment overflow");
        return 0;
    }
    for (size_t i = 0; i < archive->member_count; ++i) {
        if (!add_size(cursor, archive->members[i].size, &cursor) ||
            !align_size(cursor, 8, &cursor)) {
            set_error(error, error_size, "archive member data overflow");
            return 0;
        }
    }
    if (cursor > CVM_ARCHIVE_MAX_SIZE) {
        set_error(error, error_size, "archive exceeds maximum size");
        return 0;
    }
    uint8_t *data = calloc(cursor, 1);
    if (data == NULL) {
        set_error(error, error_size, "cannot allocate archive image");
        return 0;
    }
    memcpy(data, CVM_ARCHIVE_MAGIC, 8);
    write_u32(data + 0x08, CVM_ARCHIVE_VERSION);
    write_u32(data + 0x0C, (uint32_t)archive->member_count);
    write_u32(data + 0x10, (uint32_t)archive->symbol_count);
    write_u64(data + 0x18, ARCHIVE_HEADER_SIZE);
    write_u64(data + 0x20, ARCHIVE_HEADER_SIZE + member_bytes);
    write_u64(data + 0x28, string_offset);
    write_u64(data + 0x30, string_size);
    write_u64(data + 0x38, cursor);

    size_t string_cursor = string_offset;
    size_t data_cursor;
    (void)align_size(string_offset + string_size, 8, &data_cursor);
    for (size_t i = 0; i < archive->member_count; ++i) {
        const CvmArchiveMember *member = &archive->members[i];
        uint8_t *entry = data + ARCHIVE_HEADER_SIZE +
                         i * ARCHIVE_MEMBER_SIZE;
        write_u32(entry + 0x00,
                  (uint32_t)(string_cursor - string_offset));
        write_u64(entry + 0x08, data_cursor);
        write_u64(entry + 0x10, member->size);
        size_t name_size = strlen(member->name) + 1;
        memcpy(data + string_cursor, member->name, name_size);
        string_cursor += name_size;
        memcpy(data + data_cursor, member->data, member->size);
        data_cursor += member->size;
        (void)align_size(data_cursor, 8, &data_cursor);
    }
    for (size_t i = 0; i < archive->symbol_count; ++i) {
        const CvmArchiveSymbol *symbol = &archive->symbols[i];
        uint8_t *entry = data + ARCHIVE_HEADER_SIZE + member_bytes +
                         i * ARCHIVE_SYMBOL_SIZE;
        write_u32(entry + 0x00,
                  (uint32_t)(string_cursor - string_offset));
        write_u32(entry + 0x04, symbol->member_index);
        write_u32(entry + 0x08, symbol->flags);
        size_t name_size = strlen(symbol->name) + 1;
        memcpy(data + string_cursor, symbol->name, name_size);
        string_cursor += name_size;
    }
    FILE *file = fopen(path, "wb");
    int okay = file != NULL && fwrite(data, 1, cursor, file) == cursor;
    if (file != NULL && fclose(file) != 0) okay = 0;
    free(data);
    if (!okay) set_error(error, error_size, "cannot write archive file");
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

int cvm_archive_read(const char *path, CvmArchive *archive,
                     char *error, size_t error_size)
{
    if (archive == NULL) return 0;
    *archive = (CvmArchive){0};
    if (error != NULL && error_size != 0) error[0] = '\0';
    FILE *file = path != NULL ? fopen(path, "rb") : NULL;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        set_error(error, error_size, "cannot open archive file");
        return 0;
    }
    long measured = ftell(file);
    if (measured < (long)ARCHIVE_HEADER_SIZE ||
        (unsigned long)measured > CVM_ARCHIVE_MAX_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(error, error_size, "invalid archive file size");
        return 0;
    }
    size_t size = (size_t)measured;
    uint8_t *data = malloc(size);
    if (data == NULL || fread(data, 1, size, file) != size ||
        fclose(file) != 0) {
        free(data);
        set_error(error, error_size, "cannot read archive file");
        return 0;
    }
    uint32_t member_count = read_u32(data + 0x0C);
    uint32_t symbol_count = read_u32(data + 0x10);
    uint64_t member_offset = read_u64(data + 0x18);
    uint64_t symbol_offset = read_u64(data + 0x20);
    uint64_t string_offset64 = read_u64(data + 0x28);
    uint64_t string_size64 = read_u64(data + 0x30);
    size_t member_bytes, symbol_bytes;
    if (memcmp(data, CVM_ARCHIVE_MAGIC, 8) != 0 ||
        read_u32(data + 0x08) != CVM_ARCHIVE_VERSION ||
        read_u64(data + 0x38) != size || member_count == 0 ||
        member_count > CVM_ARCHIVE_MAX_MEMBERS ||
        symbol_count > CVM_ARCHIVE_MAX_SYMBOLS ||
        !multiply_size(member_count, ARCHIVE_MEMBER_SIZE, &member_bytes) ||
        !multiply_size(symbol_count, ARCHIVE_SYMBOL_SIZE, &symbol_bytes) ||
        !range_valid(size, member_offset, member_bytes) ||
        !range_valid(size, symbol_offset, symbol_bytes) ||
        !range_valid(size, string_offset64, string_size64) ||
        string_offset64 > SIZE_MAX || string_size64 > SIZE_MAX) {
        free(data);
        set_error(error, error_size, "invalid archive header or tables");
        return 0;
    }
    size_t string_offset = (size_t)string_offset64;
    size_t string_size = (size_t)string_size64;
    archive->members = calloc(member_count, sizeof(*archive->members));
    archive->symbols = calloc(symbol_count, sizeof(*archive->symbols));
    if (archive->members == NULL ||
        (symbol_count != 0 && archive->symbols == NULL)) {
        free(data);
        cvm_archive_destroy(archive);
        set_error(error, error_size, "cannot allocate decoded archive");
        return 0;
    }
    archive->member_count = member_count;
    archive->symbol_count = symbol_count;
    for (size_t i = 0; i < member_count; ++i) {
        const uint8_t *entry = data + (size_t)member_offset +
                               i * ARCHIVE_MEMBER_SIZE;
        uint64_t data_offset = read_u64(entry + 0x08);
        uint64_t data_size = read_u64(entry + 0x10);
        CvmArchiveMember *member = &archive->members[i];
        if (!read_name(data, size, string_offset, string_size,
                       read_u32(entry + 0x00), &member->name) ||
            data_size == 0 || data_size > CVM_OBJECT_MAX_SIZE ||
            data_size > SIZE_MAX || !range_valid(size, data_offset, data_size)) {
            free(data);
            cvm_archive_destroy(archive);
            set_error(error, error_size, "invalid archive member");
            return 0;
        }
        member->size = (size_t)data_size;
        member->data = malloc(member->size);
        if (member->data == NULL) {
            free(data);
            cvm_archive_destroy(archive);
            set_error(error, error_size, "cannot allocate archive member");
            return 0;
        }
        memcpy(member->data, data + (size_t)data_offset, member->size);
    }
    for (size_t i = 0; i < symbol_count; ++i) {
        const uint8_t *entry = data + (size_t)symbol_offset +
                               i * ARCHIVE_SYMBOL_SIZE;
        CvmArchiveSymbol *symbol = &archive->symbols[i];
        symbol->member_index = read_u32(entry + 0x04);
        symbol->flags = read_u32(entry + 0x08);
        if (!read_name(data, size, string_offset, string_size,
                       read_u32(entry + 0x00), &symbol->name)) {
            free(data);
            cvm_archive_destroy(archive);
            set_error(error, error_size, "invalid archive symbol");
            return 0;
        }
    }
    free(data);
    if (!archive_valid(archive)) {
        cvm_archive_destroy(archive);
        set_error(error, error_size, "archive contents violate format rules");
        return 0;
    }
    return 1;
}
