#include "cvmir.h"
#include "object_assembler.h"
#include "object_format.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage:\n"
            "  %s -S INPUT.ll -o OUTPUT.s [--allow-foreign-triple]\n"
            "  %s -c INPUT.ll -o OUTPUT.o [--allow-foreign-triple]\n"
            "\n"
            "Options:\n"
            "  -S                      Emit RArchM64 assembly\n"
            "  -c                      Emit a CVMOBJ2 relocatable object\n"
            "  -o FILE                 Set output path\n"
            "  --allow-foreign-triple  Bootstrap only; accept non-RArchM64 IR triple\n"
            "  --print-target          Print the RArchM64 LLVM triple and DataLayout\n"
            "  -h, --help              Show this help\n",
            program, program);
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cvmir: cannot open '%s': %s\n", path,
                strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "cvmir: cannot seek '%s'\n", path);
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cvmir: cannot determine size of '%s'\n", path);
        fclose(file);
        return NULL;
    }
    if ((unsigned long)size > 64UL * 1024UL * 1024UL) {
        fputs("cvmir: input exceeds the 64 MiB safety limit\n", stderr);
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)size + 1);
    if (data == NULL) {
        fputs("cvmir: out of memory\n", stderr);
        fclose(file);
        return NULL;
    }
    size_t read_size = fread(data, 1, (size_t)size, file);
    int close_error = fclose(file) != 0;
    if (read_size != (size_t)size || close_error) {
        fprintf(stderr, "cvmir: cannot read '%s'\n", path);
        free(data);
        return NULL;
    }
    data[size] = '\0';
    return data;
}

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cvmir: cannot open output '%s': %s\n", path,
                strerror(errno));
        return 0;
    }
    size_t length = strlen(text);
    int okay = fwrite(text, 1, length, file) == length;
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "cvmir: cannot write '%s'\n", path);
    }
    return okay;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    int emit_assembly = 0;
    int emit_object = 0;
    CvmIrOptions options;
    memset(&options, 0, sizeof(options));
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-S") == 0) {
            emit_assembly = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            emit_object = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--allow-foreign-triple") == 0) {
            options.allow_foreign_triple = 1;
        } else if (strcmp(argv[i], "--print-target") == 0) {
            printf("target triple = \"%s\"\n",
                   RARCH_M64_LLVM_TARGET_TRIPLE);
            printf("target datalayout = \"%s\"\n", CVM_LLVM_DATA_LAYOUT);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        } else if (argv[i][0] == '-' || input_path != NULL) {
            print_usage(stderr, argv[0]);
            return 2;
        } else {
            input_path = argv[i];
        }
    }
    if (input_path == NULL || output_path == NULL ||
        emit_assembly == emit_object) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    char *source = read_file(input_path);
    if (source == NULL) {
        return 1;
    }
    char *assembly = NULL;
    CvmIrError ir_error;
    if (!cvmir_translate(source, &options, &assembly, &ir_error)) {
        fprintf(stderr, "%s:%zu:%zu: error: %s\n", input_path,
                ir_error.line, ir_error.column, ir_error.message);
        free(source);
        return 1;
    }
    free(source);
    if (emit_assembly) {
        int okay = write_text(output_path, assembly);
        free(assembly);
        if (okay) {
            printf("Translated LLVM IR to RArchM64 assembly -> %s\n",
                   output_path);
        }
        return okay ? 0 : 1;
    }
    CvmObjectFile object;
    AssemblyError assembler_error;
    if (!assembler_assemble_object(assembly, &object, &assembler_error)) {
        fprintf(stderr, "cvmir: generated assembly:%zu:%zu: error: %s\n",
                assembler_error.line, assembler_error.column,
                assembler_error.message);
        free(assembly);
        return 1;
    }
    free(assembly);
    char object_error[192];
    int okay = cvm_object_write(output_path, &object, object_error,
                                sizeof(object_error));
    if (!okay) {
        fprintf(stderr, "cvmir: %s: %s\n", output_path, object_error);
    } else {
        printf("Translated LLVM IR to RArchM64 object: %zu sections, %zu symbols, "
               "%zu relocations -> %s\n", object.section_count,
               object.symbol_count, object.relocation_count, output_path);
    }
    cvm_object_destroy(&object);
    return okay ? 0 : 1;
}
