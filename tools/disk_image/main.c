#include "disk_image.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EXECUTABLE_IMAGE_SIZE ((size_t)256 * 1024 * 1024)

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s create -o DISK.img --size SIZE --bootloader BOOT.exf --kernel KERNEL.exf [--reproducible]\n"
            "  %s inspect DISK.img\n"
            "SIZE accepts an optional binary K, M or G suffix.\n",
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

static int parse_size(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0' || *text == '-') {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text) {
        return 0;
    }
    uint64_t multiplier = 1;
    if (*end != '\0') {
        if (end[1] != '\0') {
            return 0;
        }
        switch (*end) {
        case 'k':
        case 'K':
            multiplier = UINT64_C(1024);
            break;
        case 'm':
        case 'M':
            multiplier = UINT64_C(1024) * 1024;
            break;
        case 'g':
        case 'G':
            multiplier = UINT64_C(1024) * 1024 * 1024;
            break;
        default:
            return 0;
        }
    }
    if ((uint64_t)parsed > UINT64_MAX / multiplier) {
        return 0;
    }
    *value = (uint64_t)parsed * multiplier;
    return 1;
}

static uint8_t *read_image(const char *kind, const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "vmkdisk: cannot open %s '%s'\n", kind, path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "vmkdisk: cannot measure %s '%s'\n", kind, path);
        fclose(file);
        return NULL;
    }
    long measured = ftell(file);
    if (measured <= 0 ||
        (unsigned long)measured > MAX_EXECUTABLE_IMAGE_SIZE) {
        fprintf(stderr,
                "vmkdisk: %s must be between 1 and %zu bytes\n",
                kind,
                MAX_EXECUTABLE_IMAGE_SIZE);
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "vmkdisk: cannot rewind %s '%s'\n", kind, path);
        fclose(file);
        return NULL;
    }
    *size = (size_t)measured;
    uint8_t *data = malloc(*size);
    if (data == NULL) {
        fprintf(stderr, "vmkdisk: cannot allocate %s buffer\n", kind);
        fclose(file);
        return NULL;
    }
    int okay = fread(data, 1, *size, file) == *size;
    if (fclose(file) != 0) {
        okay = 0;
    }
    if (!okay) {
        fprintf(stderr, "vmkdisk: cannot read %s '%s'\n", kind, path);
        free(data);
        return NULL;
    }
    return data;
}

static int create_image(int argc, char **argv)
{
    const char *output_path = NULL;
    const char *bootloader_path = NULL;
    const char *kernel_path = NULL;
    uint64_t disk_size = 0;
    uint32_t create_flags = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &disk_size)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--bootloader") == 0 &&
                   i + 1 < argc) {
            bootloader_path = argv[++i];
        } else if (strcmp(argv[i], "--kernel") == 0 && i + 1 < argc) {
            kernel_path = argv[++i];
        } else if (strcmp(argv[i], "--reproducible") == 0) {
            create_flags |= CVM_DISK_CREATE_REPRODUCIBLE;
        } else {
            return 2;
        }
    }
    if (output_path == NULL || bootloader_path == NULL ||
        kernel_path == NULL || disk_size == 0) {
        return 2;
    }
    if (!has_exf_extension(bootloader_path) ||
        !has_exf_extension(kernel_path)) {
        fputs("vmkdisk: bootloader and kernel must be RISC-MV .exf files\n",
              stderr);
        return 2;
    }

    size_t bootloader_size;
    uint8_t *bootloader = read_image("bootloader",
                                     bootloader_path,
                                     &bootloader_size);
    if (bootloader == NULL) {
        return 1;
    }
    size_t kernel_size;
    uint8_t *kernel = read_image("kernel", kernel_path, &kernel_size);
    if (kernel == NULL) {
        free(bootloader);
        return 1;
    }
    char error[192];
    CvmDiskStatus status = cvm_disk_image_create(output_path,
                                                  disk_size,
                                                  bootloader,
                                                  bootloader_size,
                                                  kernel,
                                                  kernel_size,
                                                  create_flags,
                                                  error,
                                                  sizeof(error));
    free(bootloader);
    free(kernel);
    if (status != CVM_DISK_OK) {
        fprintf(stderr,
                "vmkdisk: create failed: %s: %s\n",
                cvm_disk_status_name(status),
                error);
        return 1;
    }

    CvmDiskImageInfo info;
    status = cvm_disk_image_inspect(output_path,
                                    &info,
                                    error,
                                    sizeof(error));
    if (status != CVM_DISK_OK) {
        fprintf(stderr,
                "vmkdisk: verification failed: %s: %s\n",
                cvm_disk_status_name(status),
                error);
        return 1;
    }
    printf("Created %s (%" PRIu64 " bytes, %" PRIu64 " sectors)\n",
           output_path,
           info.disk_size,
           info.total_sectors);
    printf("  RISC-MV boot partition: LBA %" PRIu64 " + %" PRIu64 "\n",
           info.partition_start_lba,
           info.partition_sectors);
    printf("  FAT32: %u sectors/cluster, %u clusters\n",
           info.sectors_per_cluster,
           info.cluster_count);
    printf("  /BOOT/BOOT.EXF:   %" PRIu64 " bytes, first cluster %u\n",
           info.bootloader_size,
           info.bootloader_first_cluster);
    printf("  /BOOT/KERNEL.EXF: %" PRIu64 " bytes, first cluster %u\n",
           info.kernel_size,
           info.kernel_first_cluster);
    return 0;
}

static int inspect_image(const char *path)
{
    CvmDiskImageInfo info;
    char error[192];
    CvmDiskStatus status = cvm_disk_image_inspect(path,
                                                   &info,
                                                   error,
                                                   sizeof(error));
    if (status != CVM_DISK_OK) {
        fprintf(stderr,
                "vmkdisk: inspect failed: %s: %s\n",
                cvm_disk_status_name(status),
                error);
        return 1;
    }
    printf("Valid RISC-MV GPT/FAT32 boot disk\n");
    printf("  image:              %s\n", path);
    printf("  size:               %" PRIu64 " bytes\n", info.disk_size);
    printf("  sectors:            %" PRIu64 "\n", info.total_sectors);
    printf("  boot partition LBA: %" PRIu64 "\n",
           info.partition_start_lba);
    printf("  partition sectors:  %" PRIu64 "\n",
           info.partition_sectors);
    printf("  FAT sectors:        %u\n", info.fat_sectors);
    printf("  sectors/cluster:    %u\n", info.sectors_per_cluster);
    printf("  clusters:           %u\n", info.cluster_count);
    printf("  bootloader cluster: %u\n", info.bootloader_first_cluster);
    printf("  bootloader size:    %" PRIu64 " bytes\n",
           info.bootloader_size);
    printf("  kernel cluster:     %u\n", info.kernel_first_cluster);
    printf("  kernel size:        %" PRIu64 " bytes\n", info.kernel_size);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "create") == 0) {
        int result = create_image(argc, argv);
        if (result == 2) {
            print_usage(argv[0]);
        }
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        return inspect_image(argv[2]);
    }
    print_usage(argv[0]);
    return 2;
}
