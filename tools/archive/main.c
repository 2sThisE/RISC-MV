#include "archive_format.h"
#include "object_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s create OUTPUT.a INPUT.o...\n"
            "  %s rcs OUTPUT.a INPUT.o...\n"
            "  %s list ARCHIVE.a\n"
            "  %s t ARCHIVE.a\n",
            program, program, program, program);
}

static char *copy_text(const char *text)
{
    size_t size = strlen(text);
    char *copy = malloc(size + 1);
    if (copy != NULL) memcpy(copy, text, size + 1);
    return copy;
}

static const char *base_name(const char *path)
{
    const char *name = path;
    for (const char *cursor = path; *cursor != '\0'; ++cursor)
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    return name;
}

static int read_file(const char *path, uint8_t **data, size_t *size,
                     char *error, size_t error_size)
{
    *data = NULL;
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        (void)snprintf(error, error_size, "cannot open input object");
        return 0;
    }
    long measured = ftell(file);
    if (measured <= 0 || (unsigned long)measured > CVM_OBJECT_MAX_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        (void)snprintf(error, error_size, "invalid input object size");
        return 0;
    }
    *size = (size_t)measured;
    *data = malloc(*size);
    if (*data == NULL || fread(*data, 1, *size, file) != *size ||
        fclose(file) != 0) {
        free(*data);
        *data = NULL;
        *size = 0;
        (void)snprintf(error, error_size, "cannot read input object");
        return 0;
    }
    return 1;
}

static int member_name_unique(const CvmArchive *archive, size_t index)
{
    for (size_t i = 0; i < index; ++i)
        if (strcmp(archive->members[i].name,
                   archive->members[index].name) == 0) return 0;
    return 1;
}

static int create_archive(const char *output, int input_count, char **inputs)
{
    if (input_count <= 0 || (unsigned int)input_count >
                                CVM_ARCHIVE_MAX_MEMBERS) {
        fputs("cvmar: archive requires at least one input object\n", stderr);
        return 0;
    }
    CvmArchive archive = {0};
    archive.member_count = (size_t)input_count;
    archive.members = calloc(archive.member_count, sizeof(*archive.members));
    if (archive.members == NULL) {
        fputs("cvmar: cannot allocate member table\n", stderr);
        return 0;
    }
    size_t symbol_capacity = 0;
    int okay = 1;
    for (size_t member_index = 0; member_index < archive.member_count;
         ++member_index) {
        CvmArchiveMember *member = &archive.members[member_index];
        member->name = copy_text(base_name(inputs[member_index]));
        char error[160];
        if (member->name == NULL || member->name[0] == '\0' ||
            !member_name_unique(&archive, member_index)) {
            fprintf(stderr, "cvmar: duplicate or invalid member name '%s'\n",
                    base_name(inputs[member_index]));
            okay = 0;
            break;
        }
        CvmObjectFile object;
        if (!cvm_object_read(inputs[member_index], &object, error,
                             sizeof(error))) {
            fprintf(stderr, "cvmar: %s: %s\n", inputs[member_index], error);
            okay = 0;
            break;
        }
        if (!read_file(inputs[member_index], &member->data, &member->size,
                       error, sizeof(error))) {
            fprintf(stderr, "cvmar: %s: %s\n", inputs[member_index], error);
            cvm_object_destroy(&object);
            okay = 0;
            break;
        }
        for (size_t i = 0; i < object.symbol_count; ++i) {
            const CvmObjectSymbol *input = &object.symbols[i];
            if ((input->flags & CVM_OBJECT_SYMBOL_GLOBAL) == 0 ||
                (input->flags & (CVM_OBJECT_SYMBOL_DEFINED |
                                 CVM_OBJECT_SYMBOL_COMMON)) == 0)
                continue;
            if (archive.symbol_count == symbol_capacity) {
                size_t capacity = symbol_capacity == 0
                                      ? 16 : symbol_capacity * 2;
                if (capacity > CVM_ARCHIVE_MAX_SYMBOLS) {
                    okay = 0;
                    break;
                }
                CvmArchiveSymbol *grown = realloc(
                    archive.symbols, capacity * sizeof(*grown));
                if (grown == NULL) {
                    okay = 0;
                    break;
                }
                archive.symbols = grown;
                symbol_capacity = capacity;
            }
            CvmArchiveSymbol *symbol =
                &archive.symbols[archive.symbol_count++];
            *symbol = (CvmArchiveSymbol){
                .name = copy_text(input->name),
                .member_index = (uint32_t)member_index,
                .flags = (input->flags & CVM_OBJECT_SYMBOL_ENTRY) != 0
                             ? CVM_ARCHIVE_SYMBOL_ENTRY : 0
            };
            if (symbol->name == NULL) {
                okay = 0;
                break;
            }
        }
        cvm_object_destroy(&object);
        if (!okay) {
            fputs("cvmar: cannot allocate archive symbol index\n", stderr);
            break;
        }
    }
    if (okay) {
        char error[160];
        okay = cvm_archive_write(output, &archive, error, sizeof(error));
        if (!okay) fprintf(stderr, "cvmar: %s: %s\n", output, error);
    }
    if (okay)
        printf("Created archive: %zu members, %zu symbols -> %s\n",
               archive.member_count, archive.symbol_count, output);
    cvm_archive_destroy(&archive);
    return okay;
}

static int list_archive(const char *path)
{
    CvmArchive archive;
    char error[160];
    if (!cvm_archive_read(path, &archive, error, sizeof(error))) {
        fprintf(stderr, "cvmar: %s: %s\n", path, error);
        return 0;
    }
    printf("Archive: %s (%zu members, %zu symbols)\n", path,
           archive.member_count, archive.symbol_count);
    for (size_t member = 0; member < archive.member_count; ++member) {
        printf("  %s (%zu bytes)\n", archive.members[member].name,
               archive.members[member].size);
        for (size_t symbol = 0; symbol < archive.symbol_count; ++symbol) {
            if (archive.symbols[symbol].member_index == member)
                printf("    %c %s\n",
                       (archive.symbols[symbol].flags &
                        CVM_ARCHIVE_SYMBOL_ENTRY) != 0 ? 'E' : 'G',
                       archive.symbols[symbol].name);
        }
    }
    cvm_archive_destroy(&archive);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 &&
        (strcmp(argv[1], "create") == 0 || strcmp(argv[1], "rcs") == 0)) {
        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }
        return create_archive(argv[2], argc - 3, argv + 3) ? 0 : 1;
    }
    if (argc == 3 &&
        (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "t") == 0))
        return list_archive(argv[2]) ? 0 : 1;
    usage(argv[0]);
    return 2;
}
