#include "boot_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RAW_KERNEL_SIZE ((size_t)256 * 1024 * 1024)

typedef struct {
    const char *input_path;
    const char *output_path;
    uint64_t load_address;
    uint64_t virtual_address;
    uint64_t entry_address;
    uint64_t memory_size;
    uint64_t alignment;
    uint64_t required_features;
    uint32_t segment_flags;
    uint8_t build_id[16];
    int load_set;
    int virtual_set;
    int entry_set;
    int memory_size_set;
} PackOptions;

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s pack INPUT.bin -o OUTPUT.exf --load ADDRESS [options]\n"
            "  %s inspect INPUT.exf\n"
            "Options:\n"
            "  --entry ADDRESS       Default: load address\n"
            "  --virtual ADDRESS     Default: load address\n"
            "  --memory-size SIZE    Includes zero-filled BSS\n"
            "  --alignment SIZE      Power of two, default: 4096\n"
            "  --flags rwx           Default: rx\n"
            "  --features MASK       Required System Information feature mask\n"
            "  --build-id HEX32      16-byte hexadecimal build identifier\n",
            program,
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

static int parse_flags(const char *text, uint32_t *flags)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }
    uint32_t parsed = 0;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        uint32_t bit;
        switch (*cursor) {
        case 'r':
            bit = CVM_SEGMENT_READ;
            break;
        case 'w':
            bit = CVM_SEGMENT_WRITE;
            break;
        case 'x':
            bit = CVM_SEGMENT_EXECUTE;
            break;
        default:
            return 0;
        }
        if ((parsed & bit) != 0) {
            return 0;
        }
        parsed |= bit;
    }
    if ((parsed & CVM_SEGMENT_READ) == 0) {
        return 0;
    }
    *flags = parsed;
    return 1;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int parse_build_id(const char *text, uint8_t output[16])
{
    if (text == NULL || strlen(text) != 32) {
        return 0;
    }
    for (size_t i = 0; i < 16; ++i) {
        int high = hex_digit(text[i * 2]);
        int low = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        output[i] = (uint8_t)((high << 4) | low);
    }
    return 1;
}

static uint8_t *read_file(const char *path, size_t maximum, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "vmkimg: cannot open input '%s'\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "vmkimg: cannot measure input '%s'\n", path);
        fclose(file);
        return NULL;
    }
    long measured = ftell(file);
    if (measured <= 0 || (unsigned long)measured > maximum) {
        fprintf(stderr,
                "vmkimg: input size must be between 1 and %zu bytes\n",
                maximum);
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "vmkimg: cannot rewind input '%s'\n", path);
        fclose(file);
        return NULL;
    }

    *size = (size_t)measured;
    uint8_t *data = malloc(*size);
    if (data == NULL) {
        fputs("vmkimg: cannot allocate input buffer\n", stderr);
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, *size, file) != *size) {
        fprintf(stderr, "vmkimg: cannot read input '%s'\n", path);
        free(data);
        fclose(file);
        return NULL;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "vmkimg: cannot close input '%s'\n", path);
        free(data);
        return NULL;
    }
    return data;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "vmkimg: cannot open output '%s'\n", path);
        return 0;
    }
    int okay = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "vmkimg: cannot write output '%s'\n", path);
    }
    return okay;
}

static int parse_pack_options(int argc, char **argv, PackOptions *options)
{
    memset(options, 0, sizeof(*options));
    options->alignment = 4096;
    options->segment_flags = CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE;

    if (argc < 3) {
        return 0;
    }
    options->input_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            options->output_path = argv[++i];
        } else if (strcmp(argv[i], "--load") == 0 && i + 1 < argc) {
            options->load_set = parse_u64(argv[++i], &options->load_address);
            if (!options->load_set) {
                return 0;
            }
        } else if (strcmp(argv[i], "--virtual") == 0 && i + 1 < argc) {
            options->virtual_set = parse_u64(argv[++i],
                                             &options->virtual_address);
            if (!options->virtual_set) {
                return 0;
            }
        } else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
            options->entry_set = parse_u64(argv[++i],
                                           &options->entry_address);
            if (!options->entry_set) {
                return 0;
            }
        } else if (strcmp(argv[i], "--memory-size") == 0 && i + 1 < argc) {
            options->memory_size_set = parse_u64(argv[++i],
                                                 &options->memory_size);
            if (!options->memory_size_set) {
                return 0;
            }
        } else if (strcmp(argv[i], "--alignment") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &options->alignment)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--features") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &options->required_features)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc) {
            if (!parse_flags(argv[++i], &options->segment_flags)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--build-id") == 0 && i + 1 < argc) {
            if (!parse_build_id(argv[++i], options->build_id)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return options->output_path != NULL &&
           has_exf_extension(options->output_path) && options->load_set;
}

static int pack_kernel(const PackOptions *options)
{
    size_t raw_size;
    uint8_t *raw = read_file(options->input_path,
                             MAX_RAW_KERNEL_SIZE,
                             &raw_size);
    if (raw == NULL) {
        return 1;
    }

    uint64_t virtual_address = options->virtual_set
                                   ? options->virtual_address
                                   : options->load_address;
    uint64_t entry_address = options->entry_set
                                 ? options->entry_address
                                 : options->load_address;
    uint64_t memory_size = options->memory_size_set
                               ? options->memory_size
                               : (uint64_t)raw_size;
    size_t data_offset = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE;
    if (raw_size > SIZE_MAX - data_offset) {
        free(raw);
        fputs("vmkimg: output image size overflow\n", stderr);
        return 1;
    }
    size_t image_size = data_offset + raw_size;
    uint8_t *image = calloc(1, image_size);
    if (image == NULL) {
        free(raw);
        fputs("vmkimg: cannot allocate output image\n", stderr);
        return 1;
    }

    CvmKernelHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, RISC_MV_EXF_MAGIC, 8);
    header.format_major = CVM_KERNEL_FORMAT_MAJOR;
    header.format_minor = CVM_KERNEL_FORMAT_MINOR;
    header.header_size = CVM_KERNEL_HEADER_SIZE;
    header.isa_id = RARCH_M64_ISA_ID;
    header.isa_version = CVM_ISA_VERSION;
    header.address_bits = CVM_ADDRESS_BITS;
    header.byte_order = CVM_BYTE_ORDER_LITTLE;
    header.segment_count = 1;
    header.segment_entry_size = CVM_KERNEL_SEGMENT_SIZE;
    header.segment_table_offset = CVM_KERNEL_HEADER_SIZE;
    header.entry_physical_address = entry_address;
    header.image_file_size = image_size;
    header.required_cpu_features = options->required_features;
    memcpy(header.build_id, options->build_id, sizeof(header.build_id));
    cvm_kernel_header_encode(image, &header);

    CvmKernelSegment segment;
    memset(&segment, 0, sizeof(segment));
    segment.type = CVM_SEGMENT_LOAD;
    segment.flags = options->segment_flags;
    segment.file_offset = data_offset;
    segment.load_address = options->load_address;
    segment.virtual_address = virtual_address;
    segment.file_size = raw_size;
    segment.memory_size = memory_size;
    segment.alignment = options->alignment;
    cvm_kernel_segment_encode(image + CVM_KERNEL_HEADER_SIZE, &segment);
    memcpy(image + data_offset, raw, raw_size);
    free(raw);

    CvmBootFormatStatus status = cvm_kernel_image_finalize(image, image_size);
    char error[160] = {0};
    if (status == CVM_BOOT_FORMAT_OK) {
        status = cvm_kernel_image_validate(image,
                                           image_size,
                                           NULL,
                                           error,
                                           sizeof(error));
    }
    if (status != CVM_BOOT_FORMAT_OK) {
        fprintf(stderr,
                "vmkimg: invalid output image: %s%s%s\n",
                cvm_boot_format_status_name(status),
                error[0] == '\0' ? "" : ": ",
                error);
        free(image);
        return 1;
    }
    if (!write_file(options->output_path, image, image_size)) {
        free(image);
        return 1;
    }
    free(image);

    printf("Packed %zu bytes -> %s (%zu bytes), load=0x%016" PRIx64
           ", entry=0x%016" PRIx64 ", memory=%" PRIu64 "\n",
           raw_size,
           options->output_path,
           image_size,
           options->load_address,
           entry_address,
           memory_size);
    return 0;
}

static void print_segment_flags(uint32_t flags)
{
    putchar((flags & CVM_SEGMENT_READ) != 0 ? 'r' : '-');
    putchar((flags & CVM_SEGMENT_WRITE) != 0 ? 'w' : '-');
    putchar((flags & CVM_SEGMENT_EXECUTE) != 0 ? 'x' : '-');
}

static int inspect_kernel(const char *path)
{
    size_t size;
    uint8_t *image = read_file(path, MAX_RAW_KERNEL_SIZE, &size);
    if (image == NULL) {
        return 1;
    }
    CvmKernelHeader header;
    char error[160] = {0};
    CvmBootFormatStatus status = cvm_kernel_image_validate(image,
                                                            size,
                                                            &header,
                                                            error,
                                                            sizeof(error));
    if (status != CVM_BOOT_FORMAT_OK) {
        fprintf(stderr,
                "vmkimg: %s: %s: %s\n",
                path,
                cvm_boot_format_status_name(status),
                error);
        free(image);
        return 1;
    }

    printf("RISC-MV EXF image v%u.%u\n", header.format_major,
           header.format_minor);
    printf("  file size:         %" PRIu64 "\n", header.image_file_size);
    if ((header.flags & CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL) != 0) {
        printf("  physical layout:   relocatable\n");
        printf("  entry virtual:     0x%016" PRIx64 "\n",
               header.entry_virtual_address);
        printf("  entry offset:      0x%016" PRIx64 "\n",
               header.entry_physical_address);
        printf("  virtual range:     0x%016" PRIx64 "+0x%016" PRIx64
               "\n",
               header.virtual_base,
               header.virtual_size);
    } else {
        printf("  entry physical:    0x%016" PRIx64 "\n",
               header.entry_physical_address);
    }
    printf("  required features: 0x%016" PRIx64 "\n",
           header.required_cpu_features);
    printf("  segments:          %u\n", header.segment_count);

    for (uint16_t i = 0; i < header.segment_count; ++i) {
        CvmKernelSegment segment;
        size_t offset = (size_t)header.segment_table_offset +
                        (size_t)i * CVM_KERNEL_SEGMENT_SIZE;
        cvm_kernel_segment_decode(image + offset, &segment);
        printf("  [%u] ", i);
        print_segment_flags(segment.flags);
        printf(" file=%" PRIu64 " memory=%" PRIu64
               " load=0x%016" PRIx64 " virtual=0x%016" PRIx64 "\n",
               segment.file_size,
               segment.memory_size,
               segment.load_address,
               segment.virtual_address);
    }
    free(image);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "pack") == 0) {
        PackOptions options;
        if (!parse_pack_options(argc, argv, &options)) {
            print_usage(argv[0]);
            return 2;
        }
        return pack_kernel(&options);
    }
    if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        if (!has_exf_extension(argv[2])) {
            fputs("vmkimg: RISC-MV executables require the .exf extension\n",
                  stderr);
            return 2;
        }
        return inspect_kernel(argv[2]);
    }
    print_usage(argv[0]);
    return 2;
}
