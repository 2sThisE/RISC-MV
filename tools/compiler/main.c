#include "compiler.h"
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
            "  %s -S INPUT.c -o OUTPUT.s\n"
            "  %s -c INPUT.c -o OUTPUT.o\n"
            "\n"
            "Options:\n"
            "  -S          Emit CVM assembly\n"
            "  -c          Emit a CVM relocatable object\n"
            "  -o FILE     Set the output path\n"
            "  -h, --help  Show this help\n"
            "\n"
            "This bootstrap compiler accepts C source or host-preprocessed .i "
            "input.\n",
            program, program);
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "cvmcc: cannot open '%s': %s\n", path,
                strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "cvmcc: cannot seek '%s'\n", path);
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cvmcc: cannot determine size of '%s'\n", path);
        fclose(file);
        return NULL;
    }
    if ((unsigned long)size > 64UL * 1024UL * 1024UL) {
        fprintf(stderr, "cvmcc: input exceeds the 64 MiB safety limit\n");
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)size + 1);
    if (data == NULL) {
        fputs("cvmcc: out of memory\n", stderr);
        fclose(file);
        return NULL;
    }
    size_t read_size = fread(data, 1, (size_t)size, file);
    int close_error = fclose(file) != 0;
    if (read_size != (size_t)size || close_error) {
        fprintf(stderr, "cvmcc: cannot read '%s'\n", path);
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
        fprintf(stderr, "cvmcc: cannot open output '%s': %s\n", path,
                strerror(errno));
        return 0;
    }
    size_t length = strlen(text);
    int okay = fwrite(text, 1, length, file) == length;
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "cvmcc: cannot write '%s'\n", path);
    }
    return okay;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    int emit_assembly = 0;
    int emit_object = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-S") == 0) {
            emit_assembly = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            emit_object = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
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
    CvmCompilerError compiler_error;
    if (!cvm_compile_c_source(source, &assembly, &compiler_error)) {
        fprintf(stderr, "%s:%zu:%zu: error: %s\n", input_path,
                compiler_error.line, compiler_error.column,
                compiler_error.message);
        free(source);
        return 1;
    }
    free(source);
    if (emit_assembly) {
        int okay = write_text(output_path, assembly);
        free(assembly);
        if (okay) {
            printf("Compiled CVM assembly -> %s\n", output_path);
        }
        return okay ? 0 : 1;
    }
    CvmObjectFile object;
    AssemblyError assembler_error;
    if (!assembler_assemble_object(assembly, &object, &assembler_error)) {
        fprintf(stderr, "cvmcc: generated assembly:%zu:%zu: error: %s\n",
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
        fprintf(stderr, "cvmcc: %s: %s\n", output_path, object_error);
    } else {
        printf("Compiled CVM object: %zu sections, %zu symbols, %zu "
               "relocations -> %s\n", object.section_count,
               object.symbol_count, object.relocation_count, output_path);
    }
    cvm_object_destroy(&object);
    return okay ? 0 : 1;
}
