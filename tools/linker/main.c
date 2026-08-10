#include "assembler.h"
#include "boot_format.h"
#include "object_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINKER_MAX_OBJECTS ((size_t)256)

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} TextBuffer;

typedef struct {
    const char *name;
    size_t object_index;
} GlobalDefinition;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    int nobits;
} OutputSection;

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s INPUT.o... -o OUTPUT.cvm [--base ADDRESS] "
            "[--entry SYMBOL] [--map FILE]\n",
            program);
}

static int parse_u64(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0' || *text == '-') return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0') return 0;
    *value = (uint64_t)parsed;
    return 1;
}

static int append(TextBuffer *buffer, const char *text, size_t size)
{
    if (size > SIZE_MAX - buffer->size - 1) return 0;
    size_t required = buffer->size + size + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) return 0;
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int append_string(TextBuffer *buffer, const char *text)
{
    return append(buffer, text, strlen(text));
}

static int identifier_start(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '_' || value == '.' ||
           value == '$';
}

static int identifier_part(unsigned char value)
{
    return identifier_start(value) || (value >= '0' && value <= '9');
}

static int local_symbol(const CvmObjectFile *object, const char *name,
                        size_t length)
{
    for (size_t i = 0; i < object->symbol_count; ++i) {
        const CvmObjectSymbol *symbol = &object->symbols[i];
        if ((symbol->flags & (CVM_OBJECT_SYMBOL_DEFINED |
                              CVM_OBJECT_SYMBOL_GLOBAL)) !=
                CVM_OBJECT_SYMBOL_DEFINED || strlen(symbol->name) != length)
            continue;
        if (memcmp(symbol->name, name, length) == 0) return 1;
    }
    return 0;
}

static int append_renamed_source(TextBuffer *output,
                                 const CvmObjectFile *object,
                                 size_t object_index,
                                 const char *source, size_t source_size)
{
    size_t cursor = 0;
    int in_string = 0;
    int escaped = 0;
    while (cursor < source_size) {
        unsigned char value = (unsigned char)source[cursor];
        if (in_string) {
            if (!append(output, source + cursor, 1)) return 0;
            ++cursor;
            if (escaped) escaped = 0;
            else if (value == '\\') escaped = 1;
            else if (value == '"') in_string = 0;
            continue;
        }
        if (value == ';') {
            size_t end = cursor;
            while (end < source_size && source[end] != '\n') ++end;
            if (!append(output, source + cursor, end - cursor)) return 0;
            cursor = end;
            continue;
        }
        if (value == '"') {
            in_string = 1;
            if (!append(output, source + cursor, 1)) return 0;
            ++cursor;
            continue;
        }
        if (identifier_start(value)) {
            size_t end = cursor + 1;
            while (end < source_size &&
                   identifier_part((unsigned char)source[end])) ++end;
            if (local_symbol(object, source + cursor, end - cursor)) {
                char prefix[48];
                int size = snprintf(prefix, sizeof(prefix), "__cvm_o%zu_",
                                    object_index);
                if (size < 0 || (size_t)size >= sizeof(prefix) ||
                    !append(output, prefix, (size_t)size)) return 0;
            }
            if (!append(output, source + cursor, end - cursor)) return 0;
            cursor = end;
            continue;
        }
        if (!append(output, source + cursor, 1)) return 0;
        ++cursor;
    }
    return append(output, "\n", 1);
}

static int find_symbol(const AssemblyResult *result, const char *name,
                       uint64_t *address)
{
    for (size_t i = 0; i < result->symbol_count; ++i) {
        if (strcmp(result->symbols[i].name, name) == 0) {
            *address = result->symbols[i].address;
            return 1;
        }
    }
    return 0;
}

static int global_index(const GlobalDefinition *definitions, size_t count,
                        const char *name)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(definitions[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int validate_symbols(const CvmObjectFile *objects, size_t object_count,
                            const char *cli_entry, const char **entry_out)
{
    GlobalDefinition *definitions = NULL;
    size_t count = 0, capacity = 0;
    const char *object_entry = NULL;
    int okay = 1;
    for (size_t object_index = 0; object_index < object_count && okay;
         ++object_index) {
        const CvmObjectFile *object = &objects[object_index];
        for (size_t i = 0; i < object->symbol_count; ++i) {
            const CvmObjectSymbol *symbol = &object->symbols[i];
            if ((symbol->flags & CVM_OBJECT_SYMBOL_ENTRY) != 0) {
                if (object_entry != NULL &&
                    strcmp(object_entry, symbol->name) != 0) {
                    fprintf(stderr, "cvmlink: multiple object entry symbols\n");
                    okay = 0;
                    break;
                }
                object_entry = symbol->name;
            }
            if ((symbol->flags & (CVM_OBJECT_SYMBOL_GLOBAL |
                                  CVM_OBJECT_SYMBOL_DEFINED)) !=
                (CVM_OBJECT_SYMBOL_GLOBAL | CVM_OBJECT_SYMBOL_DEFINED))
                continue;
            int previous = global_index(definitions, count, symbol->name);
            if (previous >= 0) {
                fprintf(stderr,
                        "cvmlink: duplicate global symbol '%s' in objects "
                        "%zu and %zu\n", symbol->name,
                        definitions[(size_t)previous].object_index,
                        object_index);
                okay = 0;
                break;
            }
            if (count == capacity) {
                size_t next = capacity == 0 ? 32 : capacity * 2;
                GlobalDefinition *grown =
                    realloc(definitions, next * sizeof(*grown));
                if (grown == NULL) {
                    fputs("cvmlink: cannot allocate global symbol table\n",
                          stderr);
                    okay = 0;
                    break;
                }
                definitions = grown;
                capacity = next;
            }
            definitions[count++] = (GlobalDefinition){symbol->name,
                                                       object_index};
        }
    }
    for (size_t object_index = 0; object_index < object_count && okay;
         ++object_index) {
        const CvmObjectFile *object = &objects[object_index];
        for (size_t i = 0; i < object->symbol_count; ++i) {
            const CvmObjectSymbol *symbol = &object->symbols[i];
            if ((symbol->flags & CVM_OBJECT_SYMBOL_DEFINED) == 0 &&
                global_index(definitions, count, symbol->name) < 0) {
                fprintf(stderr, "cvmlink: undefined symbol '%s' in object %zu\n",
                        symbol->name, object_index);
                okay = 0;
                break;
            }
        }
    }
    const char *entry = cli_entry != NULL ? cli_entry : object_entry;
    if (okay && entry == NULL) {
        fputs("cvmlink: no entry symbol; use .entry or --entry\n", stderr);
        okay = 0;
    }
    if (okay && global_index(definitions, count, entry) < 0) {
        fprintf(stderr, "cvmlink: entry symbol '%s' is not globally defined\n",
                entry);
        okay = 0;
    }
    if (okay) *entry_out = entry;
    free(definitions);
    return okay;
}

static int build_source(const CvmObjectFile *objects, size_t object_count,
                        uint64_t base, const char *entry, TextBuffer *source)
{
    char header[80];
    int header_size = snprintf(header, sizeof(header), ".org 0x%016" PRIx64
                               "\n", base);
    if (header_size < 0 || (size_t)header_size >= sizeof(header) ||
        !append(source, header, (size_t)header_size)) return 0;
    for (size_t section = 0; section < 4; ++section) {
        char markers[160];
        int marker_size = snprintf(markers, sizeof(markers),
            ".align 4096\n__cvmlink_s%zu_start:\n", section);
        if (marker_size < 0 || (size_t)marker_size >= sizeof(markers) ||
            !append(source, markers, (size_t)marker_size)) return 0;
        for (size_t object_index = 0; object_index < object_count;
             ++object_index) {
            if (section >= objects[object_index].section_count) continue;
            const CvmObjectSection *input =
                &objects[object_index].sections[section];
            if (!append_renamed_source(source, &objects[object_index],
                                       object_index, input->source,
                                       input->source_size)) return 0;
        }
        marker_size = snprintf(markers, sizeof(markers),
            "__cvmlink_s%zu_end:\n", section);
        if (marker_size < 0 || (size_t)marker_size >= sizeof(markers) ||
            !append(source, markers, (size_t)marker_size)) return 0;
    }
    return append_string(source, ".entry ") && append_string(source, entry) &&
           append_string(source, "\n");
}

static int write_map(const char *path, const AssemblyResult *result,
                     const OutputSection sections[4])
{
    if (path == NULL) return 1;
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        fprintf(stderr, "cvmlink: cannot open map '%s'\n", path);
        return 0;
    }
    static const char *names[4] = {".text", ".rodata", ".data", ".bss"};
    int okay = 1;
    for (size_t i = 0; i < 4; ++i) {
        if (fprintf(file, "%016" PRIx64 " %016" PRIx64 " %s\n",
                    sections[i].start, sections[i].end, names[i]) < 0)
            okay = 0;
    }
    for (size_t i = 0; i < result->symbol_count && okay; ++i) {
        if (strncmp(result->symbols[i].name, "__cvmlink_", 10) == 0 ||
            strncmp(result->symbols[i].name, "__cvm_o", 7) == 0) continue;
        if (fprintf(file, "%016" PRIx64 " %s\n",
                    result->symbols[i].address,
                    result->symbols[i].name) < 0) okay = 0;
    }
    if (fclose(file) != 0) okay = 0;
    if (!okay) fprintf(stderr, "cvmlink: cannot write map '%s'\n", path);
    return okay;
}

static int write_image(const char *path, const AssemblyResult *result,
                       const OutputSection sections[4])
{
    uint16_t segment_count = 0;
    size_t payload_size = 0;
    for (size_t i = 0; i < 4; ++i) {
        uint64_t memory_size = sections[i].end - sections[i].start;
        if (memory_size == 0) continue;
        ++segment_count;
        if (!sections[i].nobits) {
            if (memory_size > SIZE_MAX - payload_size) return 0;
            payload_size += (size_t)memory_size;
        }
    }
    if (segment_count == 0) {
        fputs("cvmlink: output has no loadable sections\n", stderr);
        return 0;
    }
    size_t table_size = (size_t)segment_count * CVM_KERNEL_SEGMENT_SIZE;
    if (payload_size > SIZE_MAX - CVM_KERNEL_HEADER_SIZE - table_size) {
        fputs("cvmlink: output image size overflow\n", stderr);
        return 0;
    }
    size_t image_size = CVM_KERNEL_HEADER_SIZE + table_size + payload_size;
    uint8_t *image = calloc(image_size, 1);
    if (image == NULL) {
        fputs("cvmlink: cannot allocate output image\n", stderr);
        return 0;
    }
    CvmKernelHeader header = {0};
    memcpy(header.magic, CVM_KERNEL_MAGIC, 8);
    header.format_major = CVM_KERNEL_FORMAT_MAJOR;
    header.format_minor = CVM_KERNEL_FORMAT_MINOR;
    header.header_size = CVM_KERNEL_HEADER_SIZE;
    header.isa_id = CVM_ISA_ID;
    header.isa_version = CVM_ISA_VERSION;
    header.address_bits = CVM_ADDRESS_BITS;
    header.byte_order = CVM_BYTE_ORDER_LITTLE;
    header.segment_count = segment_count;
    header.segment_entry_size = CVM_KERNEL_SEGMENT_SIZE;
    header.segment_table_offset = CVM_KERNEL_HEADER_SIZE;
    header.entry_physical_address = result->entry_address;
    header.image_file_size = image_size;
    cvm_kernel_header_encode(image, &header);

    size_t file_cursor = CVM_KERNEL_HEADER_SIZE + table_size;
    uint16_t output_index = 0;
    for (size_t i = 0; i < 4; ++i) {
        uint64_t memory_size = sections[i].end - sections[i].start;
        if (memory_size == 0) continue;
        CvmKernelSegment segment = {0};
        segment.type = CVM_SEGMENT_LOAD;
        segment.flags = sections[i].flags;
        segment.file_offset = file_cursor;
        segment.load_address = sections[i].start;
        segment.virtual_address = sections[i].start;
        segment.file_size = sections[i].nobits ? 0 : memory_size;
        segment.memory_size = memory_size;
        segment.alignment = 4096;
        cvm_kernel_segment_encode(
            image + CVM_KERNEL_HEADER_SIZE +
                (size_t)output_index * CVM_KERNEL_SEGMENT_SIZE,
            &segment);
        if (!sections[i].nobits) {
            uint64_t raw_offset = sections[i].start - result->base_address;
            if (raw_offset > result->size ||
                memory_size > result->size - (size_t)raw_offset) {
                free(image);
                fputs("cvmlink: internal section range error\n", stderr);
                return 0;
            }
            memcpy(image + file_cursor, result->data + (size_t)raw_offset,
                   (size_t)memory_size);
            file_cursor += (size_t)memory_size;
        }
        ++output_index;
    }
    CvmBootFormatStatus status = cvm_kernel_image_finalize(image, image_size);
    char validation_error[160] = {0};
    if (status == CVM_BOOT_FORMAT_OK)
        status = cvm_kernel_image_validate(image, image_size, NULL,
                                           validation_error,
                                           sizeof(validation_error));
    if (status != CVM_BOOT_FORMAT_OK) {
        fprintf(stderr, "cvmlink: output validation failed: %s: %s\n",
                cvm_boot_format_status_name(status), validation_error);
        free(image);
        return 0;
    }
    FILE *file = fopen(path, "wb");
    int okay = file != NULL && fwrite(image, 1, image_size, file) == image_size;
    if (file != NULL && fclose(file) != 0) okay = 0;
    free(image);
    if (!okay) fprintf(stderr, "cvmlink: cannot write '%s'\n", path);
    else printf("Linked %u segments, %zu bytes, entry=0x%016" PRIx64
                " -> %s\n", segment_count, image_size,
                result->entry_address, path);
    return okay;
}

int main(int argc, char **argv)
{
    const char *output = NULL, *map = NULL, *entry_option = NULL;
    const char *inputs[LINKER_MAX_OBJECTS];
    size_t input_count = 0;
    uint64_t base = UINT64_C(0x10000);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc)
            map = argv[++i];
        else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc)
            entry_option = argv[++i];
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &base)) {
                fputs("cvmlink: invalid --base address\n", stderr);
                return 2;
            }
        } else if (argv[i][0] == '-' || input_count == LINKER_MAX_OBJECTS) {
            usage(argv[0]);
            return 2;
        } else inputs[input_count++] = argv[i];
    }
    if (output == NULL || input_count == 0 || (base & UINT64_C(4095)) != 0) {
        if ((base & UINT64_C(4095)) != 0)
            fputs("cvmlink: --base must be page-aligned\n", stderr);
        usage(argv[0]);
        return 2;
    }
    CvmObjectFile *objects = calloc(input_count, sizeof(*objects));
    if (objects == NULL) {
        fputs("cvmlink: cannot allocate object list\n", stderr);
        return 1;
    }
    int okay = 1;
    for (size_t i = 0; i < input_count; ++i) {
        char error[160];
        if (!cvm_object_read(inputs[i], &objects[i], error, sizeof(error))) {
            fprintf(stderr, "cvmlink: %s: %s\n", inputs[i], error);
            okay = 0;
            break;
        }
    }
    const char *entry = NULL;
    if (okay) okay = validate_symbols(objects, input_count, entry_option,
                                      &entry);
    TextBuffer source = {0};
    if (okay && !build_source(objects, input_count, base, entry, &source)) {
        fputs("cvmlink: cannot construct linked assembly\n", stderr);
        okay = 0;
    }
    AssemblyResult result = {0};
    if (okay) {
        AssemblyError error;
        if (!assembler_assemble(source.data, base, &result, &error)) {
            fprintf(stderr, "cvmlink: linked source:%zu:%zu: %s\n",
                    error.line, error.column, error.message);
            okay = 0;
        }
    }
    OutputSection sections[4] = {
        {0, 0, CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE, 0},
        {0, 0, CVM_SEGMENT_READ, 0},
        {0, 0, CVM_SEGMENT_READ | CVM_SEGMENT_WRITE, 0},
        {0, 0, CVM_SEGMENT_READ | CVM_SEGMENT_WRITE, 1}
    };
    for (size_t i = 0; i < 4 && okay; ++i) {
        char start_name[48], end_name[48];
        (void)snprintf(start_name, sizeof(start_name),
                       "__cvmlink_s%zu_start", i);
        (void)snprintf(end_name, sizeof(end_name), "__cvmlink_s%zu_end", i);
        if (!find_symbol(&result, start_name, &sections[i].start) ||
            !find_symbol(&result, end_name, &sections[i].end) ||
            sections[i].end < sections[i].start) {
            fputs("cvmlink: internal output section symbol error\n", stderr);
            okay = 0;
        }
    }
    if (okay) okay = write_map(map, &result, sections);
    if (okay) okay = write_image(output, &result, sections);
    assembly_result_destroy(&result);
    free(source.data);
    for (size_t i = 0; i < input_count; ++i) cvm_object_destroy(&objects[i]);
    free(objects);
    return okay ? 0 : 1;
}
