#include "boot_format.h"
#include "archive_format.h"
#include "object_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINKER_MAX_INPUTS ((size_t)256)
#define LINKER_MAX_OBJECTS ((size_t)4096)
#define OUTPUT_SECTION_COUNT ((size_t)4)
#define PAGE_ALIGNMENT UINT64_C(4096)

typedef struct {
    const char *name;
    size_t object_index;
    size_t symbol_index;
    int common;
    uint64_t common_size;
    uint64_t common_alignment;
    uint64_t common_address;
} GlobalDefinition;

typedef struct {
    size_t input_section;
    uint64_t offset;
    int present;
} Placement;

typedef struct {
    const char *name;
    uint64_t start;
    uint64_t memory_size;
    size_t file_size;
    uint8_t *data;
    uint32_t flags;
    int nobits;
} OutputSection;

typedef struct {
    const char *path;
    CvmArchive archive;
    uint8_t *extracted;
} InputArchive;

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s INPUT.o... -o OUTPUT.exf [--base ADDRESS] "
            "[--entry SYMBOL] [--map FILE] [--physical-relocatable]\n",
            program);
}

static int has_exf_extension(const char *path)
{
    if (path == NULL) return 0;
    size_t length = strlen(path);
    if (length < 4) return 0;
    const char *extension = path + length - 4;
    return extension[0] == '.' &&
           (extension[1] == 'e' || extension[1] == 'E') &&
           (extension[2] == 'x' || extension[2] == 'X') &&
           (extension[3] == 'f' || extension[3] == 'F');
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

static int align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return 0;
    *result = (value + mask) & ~mask;
    return 1;
}

static int output_section_index(const char *name)
{
    static const char *names[OUTPUT_SECTION_COUNT] = {
        ".text", ".rodata", ".data", ".bss"
    };
    for (size_t i = 0; i < OUTPUT_SECTION_COUNT; ++i)
        if (strcmp(name, names[i]) == 0) return (int)i;
    return -1;
}

static int global_index(const GlobalDefinition *globals, size_t count,
                        const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(globals[i].name, name) == 0) return (int)i;
    return -1;
}

static int collect_globals(const CvmObjectFile *objects, size_t object_count,
                           GlobalDefinition **result, size_t *result_count,
                           const char **object_entry)
{
    GlobalDefinition *globals = NULL;
    size_t count = 0, capacity = 0;
    *object_entry = NULL;
    for (size_t object_index = 0; object_index < object_count; ++object_index) {
        const CvmObjectFile *object = &objects[object_index];
        for (size_t symbol_index = 0; symbol_index < object->symbol_count;
             ++symbol_index) {
            const CvmObjectSymbol *symbol = &object->symbols[symbol_index];
            int defined = (symbol->flags & CVM_OBJECT_SYMBOL_DEFINED) != 0;
            int common = (symbol->flags & CVM_OBJECT_SYMBOL_COMMON) != 0;
            int global = (symbol->flags & CVM_OBJECT_SYMBOL_GLOBAL) != 0;
            if ((symbol->flags & CVM_OBJECT_SYMBOL_ENTRY) != 0) {
                if (*object_entry != NULL &&
                    strcmp(*object_entry, symbol->name) != 0) {
                    fputs("cvmlink: conflicting object entry symbols\n", stderr);
                    free(globals);
                    return 0;
                }
                *object_entry = symbol->name;
            }
            if ((!defined && !common) || !global) continue;
            int existing = global_index(globals, count, symbol->name);
            int weak = (symbol->flags & CVM_OBJECT_SYMBOL_WEAK) != 0;
            int rank = common ? 2 : weak ? 1 : 3;
            if (existing >= 0) {
                GlobalDefinition *previous = &globals[existing];
                const CvmObjectSymbol *previous_symbol =
                    &objects[previous->object_index]
                         .symbols[previous->symbol_index];
                int previous_rank = previous->common ? 2 :
                    (previous_symbol->flags & CVM_OBJECT_SYMBOL_WEAK) != 0
                        ? 1 : 3;
                if (rank == 3 && previous_rank == 3) {
                    fprintf(stderr, "cvmlink: duplicate strong symbol '%s'\n",
                            symbol->name);
                    free(globals);
                    return 0;
                }
                if (common && previous->common) {
                    if (symbol->size > previous->common_size)
                        previous->common_size = symbol->size;
                    if (symbol->value > previous->common_alignment)
                        previous->common_alignment = symbol->value;
                } else if (rank > previous_rank) {
                    *previous = (GlobalDefinition){
                        .name = symbol->name,
                        .object_index = object_index,
                        .symbol_index = symbol_index,
                        .common = common,
                        .common_size = common ? symbol->size : 0,
                        .common_alignment = common ? symbol->value : 0
                    };
                }
                continue;
            }
            if (count == capacity) {
                size_t next = capacity == 0 ? 16 : capacity * 2;
                GlobalDefinition *grown =
                    realloc(globals, next * sizeof(*grown));
                if (grown == NULL) {
                    free(globals);
                    fputs("cvmlink: cannot allocate global table\n", stderr);
                    return 0;
                }
                globals = grown;
                capacity = next;
            }
            globals[count++] = (GlobalDefinition){
                .name = symbol->name,
                .object_index = object_index,
                .symbol_index = symbol_index,
                .common = common,
                .common_size = common ? symbol->size : 0,
                .common_alignment = common ? symbol->value : 0
            };
        }
    }
    for (size_t object_index = 0; object_index < object_count; ++object_index) {
        const CvmObjectFile *object = &objects[object_index];
        for (size_t i = 0; i < object->symbol_count; ++i) {
            const CvmObjectSymbol *symbol = &object->symbols[i];
            if ((symbol->flags & (CVM_OBJECT_SYMBOL_DEFINED |
                                  CVM_OBJECT_SYMBOL_COMMON)) == 0 &&
                (symbol->flags & CVM_OBJECT_SYMBOL_WEAK) == 0 &&
                global_index(globals, count, symbol->name) < 0) {
                fprintf(stderr, "cvmlink: undefined symbol '%s' in object %zu\n",
                        symbol->name, object_index);
                free(globals);
                return 0;
            }
        }
    }
    *result = globals;
    *result_count = count;
    return 1;
}

static int locate_sections(const CvmObjectFile *objects, size_t object_count,
                           uint64_t base, Placement *placements,
                           OutputSection output[OUTPUT_SECTION_COUNT])
{
    uint64_t address = base;
    for (size_t output_index = 0; output_index < OUTPUT_SECTION_COUNT;
         ++output_index) {
        if (!align_up(address, PAGE_ALIGNMENT, &address)) {
            fputs("cvmlink: output address overflow\n", stderr);
            return 0;
        }
        output[output_index].start = address;
        uint64_t cursor = 0;
        for (size_t object_index = 0; object_index < object_count;
             ++object_index) {
            const CvmObjectFile *object = &objects[object_index];
            int found = -1;
            for (size_t input = 0; input < object->section_count; ++input) {
                if (output_section_index(object->sections[input].name) ==
                    (int)output_index) {
                    if (found >= 0) {
                        fprintf(stderr,
                                "cvmlink: duplicate section '%s' in object %zu\n",
                                output[output_index].name, object_index);
                        return 0;
                    }
                    found = (int)input;
                }
            }
            if (found < 0) continue;
            const CvmObjectSection *input = &object->sections[found];
            uint32_t expected_flags = output[output_index].nobits
                                          ? CVM_OBJECT_SECTION_NOBITS : 0;
            if (((input->flags & CVM_OBJECT_SECTION_NOBITS) != 0) !=
                (expected_flags != 0)) {
                fprintf(stderr, "cvmlink: incompatible section '%s'\n",
                        input->name);
                return 0;
            }
            if (!align_up(cursor, input->alignment, &cursor) ||
                input->memory_size > UINT64_MAX - cursor) {
                fputs("cvmlink: section layout overflow\n", stderr);
                return 0;
            }
            Placement *placement =
                &placements[object_index * OUTPUT_SECTION_COUNT + output_index];
            *placement = (Placement){
                .input_section = (size_t)found,
                .offset = cursor,
                .present = 1
            };
            cursor += input->memory_size;
        }
        output[output_index].memory_size = cursor;
        output[output_index].file_size = output[output_index].nobits
                                             ? 0 : (size_t)cursor;
        if (!output[output_index].nobits && cursor > SIZE_MAX) {
            fputs("cvmlink: host cannot represent output section size\n", stderr);
            return 0;
        }
        if (output[output_index].file_size != 0) {
            output[output_index].data =
                calloc(output[output_index].file_size, 1);
            if (output[output_index].data == NULL) {
                fputs("cvmlink: cannot allocate output section\n", stderr);
                return 0;
            }
        }
        if (cursor > UINT64_MAX - address) {
            fputs("cvmlink: output address overflow\n", stderr);
            return 0;
        }
        address += cursor;
    }
    for (size_t object_index = 0; object_index < object_count; ++object_index) {
        const CvmObjectFile *object = &objects[object_index];
        for (size_t output_index = 0; output_index < OUTPUT_SECTION_COUNT;
             ++output_index) {
            const Placement *placement =
                &placements[object_index * OUTPUT_SECTION_COUNT + output_index];
            if (!placement->present || output[output_index].nobits) continue;
            const CvmObjectSection *input =
                &object->sections[placement->input_section];
            if (placement->offset > output[output_index].file_size ||
                input->file_size > output[output_index].file_size -
                                       (size_t)placement->offset) {
                fputs("cvmlink: internal section copy range error\n", stderr);
                return 0;
            }
            memcpy(output[output_index].data + (size_t)placement->offset,
                   input->data, input->file_size);
        }
    }
    return 1;
}

static const Placement *placement_for_input(const CvmObjectFile *object,
                                             size_t object_index,
                                             size_t input_section,
                                             const Placement *placements)
{
    int output = output_section_index(object->sections[input_section].name);
    if (output < 0) return NULL;
    const Placement *placement =
        &placements[object_index * OUTPUT_SECTION_COUNT + (size_t)output];
    return placement->present && placement->input_section == input_section
               ? placement : NULL;
}

static int allocate_common_symbols(GlobalDefinition *globals,
                                   size_t global_count,
                                   OutputSection output[OUTPUT_SECTION_COUNT])
{
    OutputSection *bss = &output[3];
    uint64_t cursor = bss->memory_size;
    for (size_t i = 0; i < global_count; ++i) {
        if (!globals[i].common) continue;
        uint64_t aligned;
        if (bss->start > UINT64_MAX - cursor ||
            !align_up(bss->start + cursor,
                      globals[i].common_alignment, &aligned) ||
            aligned < bss->start) {
            fputs("cvmlink: common symbol allocation overflow\n", stderr);
            return 0;
        }
        cursor = aligned - bss->start;
        if (globals[i].common_size > UINT64_MAX - cursor) {
            fputs("cvmlink: common symbol allocation overflow\n", stderr);
            return 0;
        }
        globals[i].common_address = aligned;
        cursor += globals[i].common_size;
    }
    bss->memory_size = cursor;
    return 1;
}

static int symbol_address(const CvmObjectFile *objects, size_t object_index,
                          size_t symbol_index,
                          const GlobalDefinition *globals,
                          size_t global_count, const Placement *placements,
                          const OutputSection output[OUTPUT_SECTION_COUNT],
                          uint64_t *address)
{
    const CvmObjectSymbol *symbol =
        &objects[object_index].symbols[symbol_index];
    if ((symbol->flags & CVM_OBJECT_SYMBOL_GLOBAL) != 0) {
        int index = global_index(globals, global_count, symbol->name);
        if (index < 0) {
            if ((symbol->flags & CVM_OBJECT_SYMBOL_WEAK) != 0) {
                *address = 0;
                return 1;
            }
            return 0;
        }
        const GlobalDefinition *definition = &globals[index];
        if (definition->common) {
            *address = definition->common_address;
            return 1;
        }
        if (definition->object_index != object_index ||
            definition->symbol_index != symbol_index)
            return symbol_address(objects, definition->object_index,
                                  definition->symbol_index, globals,
                                  global_count, placements, output, address);
    }
    if ((symbol->flags & CVM_OBJECT_SYMBOL_DEFINED) == 0) {
        int index = global_index(globals, global_count, symbol->name);
        if (index < 0) {
            if ((symbol->flags & CVM_OBJECT_SYMBOL_WEAK) != 0) {
                *address = 0;
                return 1;
            }
            return 0;
        }
        return symbol_address(objects, globals[index].object_index,
                              globals[index].symbol_index, globals,
                              global_count, placements, output, address);
    }
    if (symbol->section_index == CVM_OBJECT_ABSOLUTE_SECTION) {
        *address = symbol->value;
        return 1;
    }
    const Placement *placement = placement_for_input(
        &objects[object_index], object_index, symbol->section_index,
        placements);
    int output_index = output_section_index(
        objects[object_index].sections[symbol->section_index].name);
    if (placement == NULL || output_index < 0 ||
        symbol->value > UINT64_MAX - placement->offset ||
        output[output_index].start >
            UINT64_MAX - placement->offset - symbol->value) return 0;
    *address = output[output_index].start + placement->offset + symbol->value;
    return 1;
}

static int add_signed(uint64_t value, int64_t addend, uint64_t *result)
{
    if (addend >= 0) {
        uint64_t amount = (uint64_t)addend;
        if (value > UINT64_MAX - amount) return 0;
        *result = value + amount;
    } else {
        uint64_t amount = (uint64_t)(-(addend + 1)) + 1;
        if (value < amount) return 0;
        *result = value - amount;
    }
    return 1;
}

static void write_le(uint8_t *destination, uint64_t value, size_t width)
{
    for (size_t i = 0; i < width; ++i)
        destination[i] = (uint8_t)(value >> (i * 8));
}

static int apply_relocations(const CvmObjectFile *objects,
                             size_t object_count,
                             const GlobalDefinition *globals,
                             size_t global_count,
                             const Placement *placements,
                             OutputSection output[OUTPUT_SECTION_COUNT])
{
    for (size_t object_index = 0; object_index < object_count; ++object_index) {
        const CvmObjectFile *object = &objects[object_index];
        for (size_t i = 0; i < object->relocation_count; ++i) {
            const CvmObjectRelocation *relocation = &object->relocations[i];
            int output_index = output_section_index(
                object->sections[relocation->section_index].name);
            const Placement *placement = placement_for_input(
                object, object_index, relocation->section_index, placements);
            size_t width = cvm_object_relocation_width(relocation->type);
            if (output_index < 0 || placement == NULL ||
                output[output_index].nobits ||
                relocation->offset > SIZE_MAX - placement->offset) {
                fputs("cvmlink: invalid relocation placement\n", stderr);
                return 0;
            }
            size_t field = (size_t)(placement->offset + relocation->offset);
            if (field > output[output_index].file_size ||
                width > output[output_index].file_size - field) {
                fputs("cvmlink: relocation is outside output section\n", stderr);
                return 0;
            }
            uint64_t symbol, target;
            if (!symbol_address(objects, object_index,
                                relocation->symbol_index, globals,
                                global_count, placements, output, &symbol) ||
                !add_signed(symbol, relocation->addend, &target)) {
                fprintf(stderr, "cvmlink: cannot resolve relocation in "
                        "object %zu\n", object_index);
                return 0;
            }
            uint64_t place = output[output_index].start + field;
            uint64_t encoded = target;
            if (relocation->type == CVM_OBJECT_RELOCATION_ABS32) {
                if (target > UINT32_MAX) {
                    fprintf(stderr, "cvmlink: ABS32 relocation overflow for "
                            "'%s'\n",
                            object->symbols[relocation->symbol_index].name);
                    return 0;
                }
            } else if (relocation->type == CVM_OBJECT_RELOCATION_REL32) {
                uint64_t next = place + 4;
                if (target >= next) {
                    uint64_t distance = target - next;
                    if (distance > INT32_MAX) {
                        fputs("cvmlink: REL32 relocation overflow\n", stderr);
                        return 0;
                    }
                    encoded = distance;
                } else {
                    uint64_t distance = next - target;
                    if (distance > UINT64_C(0x80000000)) {
                        fputs("cvmlink: REL32 relocation overflow\n", stderr);
                        return 0;
                    }
                    encoded = UINT32_C(0) - (uint32_t)distance;
                }
            }
            write_le(output[output_index].data + field, encoded, width);
        }
    }
    return 1;
}

static int write_map(const char *path, const CvmObjectFile *objects,
                     const GlobalDefinition *globals, size_t global_count,
                     const Placement *placements,
                     const OutputSection output[OUTPUT_SECTION_COUNT])
{
    if (path == NULL) return 1;
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        fprintf(stderr, "cvmlink: cannot open map '%s'\n", path);
        return 0;
    }
    int okay = 1;
    for (size_t i = 0; i < OUTPUT_SECTION_COUNT; ++i) {
        if (fprintf(file, "%016" PRIx64 " %016" PRIx64 " %s\n",
                    output[i].start, output[i].start + output[i].memory_size,
                    output[i].name) < 0) okay = 0;
    }
    for (size_t i = 0; i < global_count && okay; ++i) {
        uint64_t address;
        if (!symbol_address(objects, globals[i].object_index,
                            globals[i].symbol_index, globals, global_count,
                            placements, output, &address) ||
            fprintf(file, "%016" PRIx64 " %s\n", address,
                    globals[i].name) < 0) okay = 0;
    }
    if (fclose(file) != 0) okay = 0;
    if (!okay) fprintf(stderr, "cvmlink: cannot write map '%s'\n", path);
    return okay;
}

static int write_image(const char *path,
                       const OutputSection output[OUTPUT_SECTION_COUNT],
                       uint64_t entry,
                       uint64_t virtual_base,
                       int physical_relocatable)
{
    uint16_t segment_count = 0;
    size_t payload_size = 0;
    for (size_t i = 0; i < OUTPUT_SECTION_COUNT; ++i) {
        if (output[i].memory_size == 0) continue;
        ++segment_count;
        if (output[i].file_size > SIZE_MAX - payload_size) return 0;
        payload_size += output[i].file_size;
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
    memcpy(header.magic, RISC_VM_EXF_MAGIC, 8);
    header.format_major = CVM_KERNEL_FORMAT_MAJOR;
    header.format_minor = CVM_KERNEL_FORMAT_MINOR;
    header.header_size = CVM_KERNEL_HEADER_SIZE;
    header.isa_id = RISC_VM_ISA_ID;
    header.isa_version = CVM_ISA_VERSION;
    header.address_bits = CVM_ADDRESS_BITS;
    header.byte_order = CVM_BYTE_ORDER_LITTLE;
    header.segment_count = segment_count;
    header.segment_entry_size = CVM_KERNEL_SEGMENT_SIZE;
    header.segment_table_offset = CVM_KERNEL_HEADER_SIZE;
    if (physical_relocatable) {
        uint64_t virtual_end = virtual_base;
        for (size_t i = 0; i < OUTPUT_SECTION_COUNT; ++i) {
            if (output[i].memory_size == 0) continue;
            if (output[i].start > UINT64_MAX - output[i].memory_size) {
                free(image);
                return 0;
            }
            uint64_t end = output[i].start + output[i].memory_size;
            if (end > virtual_end) virtual_end = end;
        }
        header.flags = CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL;
        header.entry_physical_address = entry - virtual_base;
        header.entry_virtual_address = entry;
        header.virtual_base = virtual_base;
        header.virtual_size = virtual_end - virtual_base;
    } else {
        header.entry_physical_address = entry;
    }
    header.image_file_size = image_size;
    cvm_kernel_header_encode(image, &header);

    size_t file_cursor = CVM_KERNEL_HEADER_SIZE + table_size;
    uint16_t segment_index = 0;
    for (size_t i = 0; i < OUTPUT_SECTION_COUNT; ++i) {
        if (output[i].memory_size == 0) continue;
        CvmKernelSegment segment = {
            .type = CVM_SEGMENT_LOAD,
            .flags = output[i].flags,
            .file_offset = file_cursor,
            .load_address = physical_relocatable
                                ? output[i].start - virtual_base
                                : output[i].start,
            .virtual_address = output[i].start,
            .file_size = output[i].file_size,
            .memory_size = output[i].memory_size,
            .alignment = PAGE_ALIGNMENT
        };
        cvm_kernel_segment_encode(
            image + CVM_KERNEL_HEADER_SIZE +
                (size_t)segment_index * CVM_KERNEL_SEGMENT_SIZE,
            &segment);
        if (output[i].file_size != 0) {
            memcpy(image + file_cursor, output[i].data, output[i].file_size);
            file_cursor += output[i].file_size;
        }
        ++segment_index;
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
                " -> %s\n", segment_count, image_size, entry, path);
    return okay;
}

static int read_magic(const char *path, uint8_t magic[8])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    int okay = fread(magic, 1, 8, file) == 8;
    if (fclose(file) != 0) okay = 0;
    return okay;
}

static int object_defines(const CvmObjectFile *objects, size_t object_count,
                          const char *name)
{
    for (size_t object_index = 0; object_index < object_count; ++object_index)
        for (size_t symbol_index = 0;
             symbol_index < objects[object_index].symbol_count;
             ++symbol_index) {
            const CvmObjectSymbol *symbol =
                &objects[object_index].symbols[symbol_index];
            if ((symbol->flags & CVM_OBJECT_SYMBOL_GLOBAL) != 0 &&
                (symbol->flags & (CVM_OBJECT_SYMBOL_DEFINED |
                                  CVM_OBJECT_SYMBOL_COMMON)) != 0 &&
                strcmp(symbol->name, name) == 0) return 1;
        }
    return 0;
}

static int object_needs(const CvmObjectFile *objects, size_t object_count,
                        const char *name)
{
    if (object_defines(objects, object_count, name)) return 0;
    for (size_t object_index = 0; object_index < object_count; ++object_index)
        for (size_t symbol_index = 0;
             symbol_index < objects[object_index].symbol_count;
             ++symbol_index) {
            const CvmObjectSymbol *symbol =
                &objects[object_index].symbols[symbol_index];
            if ((symbol->flags & CVM_OBJECT_SYMBOL_DEFINED) == 0 &&
                (symbol->flags & CVM_OBJECT_SYMBOL_COMMON) == 0 &&
                (symbol->flags & CVM_OBJECT_SYMBOL_WEAK) == 0 &&
                strcmp(symbol->name, name) == 0) return 1;
        }
    return 0;
}

static int objects_have_entry(const CvmObjectFile *objects,
                              size_t object_count)
{
    for (size_t object_index = 0; object_index < object_count; ++object_index)
        for (size_t symbol_index = 0;
             symbol_index < objects[object_index].symbol_count;
             ++symbol_index)
            if ((objects[object_index].symbols[symbol_index].flags &
                 CVM_OBJECT_SYMBOL_ENTRY) != 0) return 1;
    return 0;
}

static int append_object(CvmObjectFile **objects, size_t *object_count,
                         size_t *object_capacity, CvmObjectFile *object)
{
    if (*object_count == LINKER_MAX_OBJECTS) return 0;
    if (*object_count == *object_capacity) {
        size_t capacity = *object_capacity == 0 ? 16 : *object_capacity * 2;
        if (capacity > LINKER_MAX_OBJECTS) capacity = LINKER_MAX_OBJECTS;
        CvmObjectFile *grown = realloc(*objects,
                                      capacity * sizeof(*grown));
        if (grown == NULL) return 0;
        *objects = grown;
        *object_capacity = capacity;
    }
    (*objects)[(*object_count)++] = *object;
    *object = (CvmObjectFile){0};
    return 1;
}

static int extract_archives(CvmObjectFile **objects, size_t *object_count,
                            size_t *object_capacity,
                            InputArchive *archives, size_t archive_count,
                            const char *entry_option)
{
    int progress;
    do {
        progress = 0;
        for (size_t archive_index = 0; archive_index < archive_count;
             ++archive_index) {
            InputArchive *input = &archives[archive_index];
            for (size_t symbol_index = 0;
                 symbol_index < input->archive.symbol_count; ++symbol_index) {
                const CvmArchiveSymbol *symbol =
                    &input->archive.symbols[symbol_index];
                if (input->extracted[symbol->member_index]) continue;
                int needed = object_needs(*objects, *object_count,
                                          symbol->name);
                if (!needed && entry_option != NULL &&
                    strcmp(entry_option, symbol->name) == 0 &&
                    !object_defines(*objects, *object_count, symbol->name))
                    needed = 1;
                if (!needed && entry_option == NULL &&
                    !objects_have_entry(*objects, *object_count) &&
                    (symbol->flags & CVM_ARCHIVE_SYMBOL_ENTRY) != 0)
                    needed = 1;
                if (!needed) continue;
                const CvmArchiveMember *member =
                    &input->archive.members[symbol->member_index];
                CvmObjectFile object;
                char error[160];
                if (!cvm_object_decode(member->data, member->size, &object,
                                       error, sizeof(error))) {
                    fprintf(stderr, "cvmlink: %s(%s): %s\n", input->path,
                            member->name, error);
                    return 0;
                }
                if (!append_object(objects, object_count, object_capacity,
                                   &object)) {
                    cvm_object_destroy(&object);
                    fputs("cvmlink: too many objects or allocation failure\n",
                          stderr);
                    return 0;
                }
                input->extracted[symbol->member_index] = 1;
                printf("Extracted %s(%s) for %s\n", input->path,
                       member->name, symbol->name);
                progress = 1;
                break;
            }
        }
    } while (progress);
    return 1;
}

int main(int argc, char **argv)
{
    const char *output_path = NULL, *map_path = NULL, *entry_option = NULL;
    const char *inputs[LINKER_MAX_INPUTS];
    size_t input_count = 0;
    uint64_t base = UINT64_C(0x10000);
    int physical_relocatable = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc)
            map_path = argv[++i];
        else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc)
            entry_option = argv[++i];
        else if (strcmp(argv[i], "--physical-relocatable") == 0)
            physical_relocatable = 1;
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &base)) {
                fputs("cvmlink: invalid --base address\n", stderr);
                return 2;
            }
        } else if (argv[i][0] == '-' || input_count == LINKER_MAX_INPUTS) {
            usage(argv[0]);
            return 2;
        } else {
            inputs[input_count++] = argv[i];
        }
    }
    if (output_path == NULL || input_count == 0 ||
        (base & (PAGE_ALIGNMENT - 1)) != 0) {
        if ((base & (PAGE_ALIGNMENT - 1)) != 0)
            fputs("cvmlink: --base must be page-aligned\n", stderr);
        usage(argv[0]);
        return 2;
    }
    if (!has_exf_extension(output_path)) {
        fputs("cvmlink: RISC-VM executables require the .exf extension\n",
              stderr);
        return 2;
    }

    CvmObjectFile *objects = NULL;
    size_t object_count = 0, object_capacity = 0;
    InputArchive *archives = calloc(input_count, sizeof(*archives));
    size_t archive_count = 0;
    if (archives == NULL) {
        fputs("cvmlink: cannot allocate input state\n", stderr);
        return 1;
    }
    int okay = 1;
    for (size_t i = 0; i < input_count; ++i) {
        uint8_t magic[8];
        char error[160];
        if (!read_magic(inputs[i], magic)) {
            fprintf(stderr, "cvmlink: cannot read '%s'\n", inputs[i]);
            okay = 0;
            break;
        }
        if (memcmp(magic, CVM_OBJECT_MAGIC, 8) == 0) {
            CvmObjectFile object;
            if (!cvm_object_read(inputs[i], &object, error, sizeof(error)) ||
                !append_object(&objects, &object_count, &object_capacity,
                               &object)) {
                fprintf(stderr, "cvmlink: %s: %s\n", inputs[i],
                        error[0] != '\0' ? error :
                        "too many objects or allocation failure");
                if (error[0] == '\0') cvm_object_destroy(&object);
                okay = 0;
                break;
            }
        } else if (memcmp(magic, CVM_ARCHIVE_MAGIC, 8) == 0) {
            InputArchive *input = &archives[archive_count];
            input->path = inputs[i];
            if (!cvm_archive_read(inputs[i], &input->archive, error,
                                  sizeof(error))) {
                fprintf(stderr, "cvmlink: %s: %s\n", inputs[i], error);
                okay = 0;
                break;
            }
            input->extracted = calloc(input->archive.member_count, 1);
            if (input->extracted == NULL) {
                fputs("cvmlink: cannot allocate archive extraction map\n",
                      stderr);
                cvm_archive_destroy(&input->archive);
                okay = 0;
                break;
            }
            ++archive_count;
        } else {
            fprintf(stderr, "cvmlink: '%s' is not a CVM object or archive\n",
                    inputs[i]);
            okay = 0;
            break;
        }
    }
    if (okay) okay = extract_archives(&objects, &object_count,
                                      &object_capacity, archives,
                                      archive_count, entry_option);
    Placement *placements = okay
        ? calloc(object_count * OUTPUT_SECTION_COUNT, sizeof(*placements))
        : NULL;
    if (okay && (object_count == 0 || placements == NULL)) {
        fputs("cvmlink: no objects selected or allocation failure\n", stderr);
        okay = 0;
    }
    GlobalDefinition *globals = NULL;
    size_t global_count = 0;
    const char *object_entry = NULL;
    if (okay) okay = collect_globals(objects, object_count, &globals,
                                     &global_count, &object_entry);
    const char *entry_name = entry_option != NULL ? entry_option : object_entry;
    if (okay && entry_name == NULL) {
        fputs("cvmlink: no entry symbol; use .entry or --entry\n", stderr);
        okay = 0;
    }
    int entry_global = okay ? global_index(globals, global_count, entry_name) : -1;
    if (okay && entry_global < 0) {
        fprintf(stderr, "cvmlink: entry symbol '%s' is not globally defined\n",
                entry_name);
        okay = 0;
    }
    if (okay) {
        const GlobalDefinition *definition = &globals[entry_global];
        const CvmObjectSymbol *symbol =
            &objects[definition->object_index].symbols[definition->symbol_index];
        if (symbol->section_index == CVM_OBJECT_ABSOLUTE_SECTION ||
            symbol->section_index >=
                objects[definition->object_index].section_count ||
            output_section_index(objects[definition->object_index]
                                     .sections[symbol->section_index].name) != 0) {
            fprintf(stderr, "cvmlink: entry symbol '%s' is not in .text\n",
                    entry_name);
            okay = 0;
        }
    }
    OutputSection sections[OUTPUT_SECTION_COUNT] = {
        {.name = ".text", .flags = CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE},
        {.name = ".rodata", .flags = CVM_SEGMENT_READ},
        {.name = ".data", .flags = CVM_SEGMENT_READ | CVM_SEGMENT_WRITE},
        {.name = ".bss", .flags = CVM_SEGMENT_READ | CVM_SEGMENT_WRITE,
         .nobits = 1}
    };
    if (okay) okay = locate_sections(objects, object_count, base, placements,
                                     sections);
    if (okay) okay = allocate_common_symbols(globals, global_count, sections);
    if (okay) okay = apply_relocations(objects, object_count, globals,
                                       global_count, placements, sections);
    uint64_t entry_address = 0;
    if (okay) {
        okay = symbol_address(objects, globals[entry_global].object_index,
                              globals[entry_global].symbol_index, globals,
                              global_count, placements, sections,
                              &entry_address);
        if (!okay) fputs("cvmlink: cannot resolve entry address\n", stderr);
    }
    if (okay) okay = write_map(map_path, objects, globals, global_count,
                               placements, sections);
    if (okay) okay = write_image(output_path,
                                 sections,
                                 entry_address,
                                 base,
                                 physical_relocatable);

    for (size_t i = 0; i < OUTPUT_SECTION_COUNT; ++i) free(sections[i].data);
    free(globals);
    free(placements);
    for (size_t i = 0; i < object_count; ++i) cvm_object_destroy(&objects[i]);
    free(objects);
    for (size_t i = 0; i < archive_count; ++i) {
        free(archives[i].extracted);
        cvm_archive_destroy(&archives[i].archive);
    }
    free(archives);
    return okay ? 0 : 1;
}
