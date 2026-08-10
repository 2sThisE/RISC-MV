#include "assembler.h"
#include "object_assembler.h"
#include "object_format.h"
#include "preprocessor.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s INPUT.s -c -o OUTPUT.o\n"
            "  %s INPUT.asm -o OUTPUT.bin [--base ADDRESS] "
            "[--symbols FILE] [--listing FILE]\n",
            program,
            program);
}

static int parse_address(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0' || *text == '-') {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0') {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

static char *read_source(const char *path)
{
    char *source = NULL;
    AssemblyError error;
    if (!asm_preprocess_file(path, &source, &error)) {
        fprintf(stderr, "%s:%zu:%zu: error: %s\n", path, error.line,
                error.column, error.message);
    }
    return source;
}

static int write_binary(const char *path, const AssemblyResult *result)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "vmasm: cannot open output '%s'\n", path);
        return 0;
    }
    int okay = result->size == 0 ||
               fwrite(result->data, 1, result->size, file) == result->size;
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "vmasm: cannot write output '%s'\n", path);
    }
    return okay;
}

static int write_symbols(const char *path, const AssemblyResult *result)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        fprintf(stderr, "vmasm: cannot open symbol file '%s'\n", path);
        return 0;
    }
    int okay = 1;
    for (size_t i = 0; i < result->symbol_count; ++i) {
        if (fprintf(file,
                    "0x%016" PRIx64 " %s\n",
                    result->symbols[i].address,
                    result->symbols[i].name) < 0) {
            okay = 0;
            break;
        }
    }
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "vmasm: cannot write symbol file '%s'\n", path);
    }
    return okay;
}

static int write_listing(const char *path, const AssemblyResult *result)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        fprintf(stderr, "vmasm: cannot open listing file '%s'\n", path);
        return 0;
    }
    int okay = 1;
    for (size_t offset = 0; offset < result->size; offset += 16) {
        if (fprintf(file,
                    "%016" PRIx64 "  ",
                    result->base_address + (uint64_t)offset) < 0) {
            okay = 0;
            break;
        }
        size_t line_size = result->size - offset;
        if (line_size > 16) {
            line_size = 16;
        }
        for (size_t i = 0; i < line_size; ++i) {
            if (fprintf(file, "%02x%s", result->data[offset + i],
                        i + 1 == line_size ? "" : " ") < 0) {
                okay = 0;
                break;
            }
        }
        if (!okay || fputc('\n', file) == EOF) {
            okay = 0;
            break;
        }
    }
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "vmasm: cannot write listing file '%s'\n", path);
    }
    return okay;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *symbol_path = NULL;
    const char *listing_path = NULL;
    uint64_t base_address = 1;
    int object_mode = 0;
    int base_specified = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0) {
            object_mode = 1;
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!parse_address(argv[++i], &base_address)) {
                fputs("vmasm: --base requires a valid nonnegative address\n",
                      stderr);
                return 2;
            }
            base_specified = 1;
        } else if (strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
            symbol_path = argv[++i];
        } else if (strcmp(argv[i], "--listing") == 0 && i + 1 < argc) {
            listing_path = argv[++i];
        } else if (argv[i][0] == '-' || input_path != NULL) {
            print_usage(argv[0]);
            return 2;
        } else {
            input_path = argv[i];
        }
    }
    if (input_path == NULL || output_path == NULL) {
        print_usage(argv[0]);
        return 2;
    }
    if (object_mode && (base_specified || symbol_path != NULL ||
                        listing_path != NULL)) {
        fputs("vmasm: -c cannot be combined with --base, --symbols, or "
              "--listing\n", stderr);
        return 2;
    }

    char *source = read_source(input_path);
    if (source == NULL) {
        return 1;
    }
    if (object_mode) {
        CvmObjectFile object;
        AssemblyError error;
        if (!assembler_assemble_object(source, &object, &error)) {
            fprintf(stderr, "%s:%zu:%zu: error: %s\n", input_path,
                    error.line, error.column, error.message);
            free(source);
            return 1;
        }
        free(source);
        char object_error[160];
        int okay = cvm_object_write(output_path, &object, object_error,
                                    sizeof(object_error));
        if (!okay) {
            fprintf(stderr, "vmasm: %s: %s\n", output_path, object_error);
        } else {
            printf("Assembled relocatable object: %zu sections, %zu symbols, "
                   "%zu relocations -> %s\n", object.section_count,
                   object.symbol_count, object.relocation_count, output_path);
        }
        cvm_object_destroy(&object);
        return okay ? 0 : 1;
    }
    AssemblyResult result;
    AssemblyError error;
    if (!assembler_assemble(source, base_address, &result, &error)) {
        fprintf(stderr,
                "%s:%zu:%zu: error: %s\n",
                input_path,
                error.line,
                error.column,
                error.message);
        free(source);
        return 1;
    }
    free(source);

    int okay = write_binary(output_path, &result);
    if (okay && symbol_path != NULL) {
        okay = write_symbols(symbol_path, &result);
    }
    if (okay && listing_path != NULL) {
        okay = write_listing(listing_path, &result);
    }
    if (okay) {
        printf("Assembled %zu bytes at 0x%016" PRIx64
               ", entry 0x%016" PRIx64 " -> %s\n",
               result.size,
               result.base_address,
               result.entry_address,
               output_path);
    }
    assembly_result_destroy(&result);
    return okay ? 0 : 1;
}
