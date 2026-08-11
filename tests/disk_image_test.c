#include "boot_format.h"
#include "disk_image.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *make_test_kernel(size_t *image_size)
{
    static const uint8_t code[] = {0x00, 0x01, 0x02, 0x03};
    *image_size = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE +
                  sizeof(code);
    uint8_t *image = calloc(1, *image_size);
    assert(image != NULL);

    CvmKernelHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, RISC_VM_EXF_MAGIC, 8);
    header.format_major = CVM_KERNEL_FORMAT_MAJOR;
    header.format_minor = CVM_KERNEL_FORMAT_MINOR;
    header.header_size = CVM_KERNEL_HEADER_SIZE;
    header.isa_id = RISC_VM_ISA_ID;
    header.isa_version = CVM_ISA_VERSION;
    header.address_bits = CVM_ADDRESS_BITS;
    header.byte_order = CVM_BYTE_ORDER_LITTLE;
    header.segment_count = 1;
    header.segment_entry_size = CVM_KERNEL_SEGMENT_SIZE;
    header.segment_table_offset = CVM_KERNEL_HEADER_SIZE;
    header.entry_physical_address = 0x10000;
    header.image_file_size = *image_size;
    cvm_kernel_header_encode(image, &header);

    CvmKernelSegment segment = {
        .type = CVM_SEGMENT_LOAD,
        .flags = CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE,
        .file_offset = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE,
        .load_address = 0x10000,
        .virtual_address = 0x10000,
        .file_size = sizeof(code),
        .memory_size = 4096,
        .alignment = 4096
    };
    cvm_kernel_segment_encode(image + CVM_KERNEL_HEADER_SIZE, &segment);
    memcpy(image + segment.file_offset, code, sizeof(code));
    assert(cvm_kernel_image_finalize(image, *image_size) ==
           CVM_BOOT_FORMAT_OK);
    return image;
}

static void flip_byte(const char *path, long offset)
{
    FILE *file = fopen(path, "rb+");
    assert(file != NULL);
    assert(fseek(file, offset, SEEK_SET) == 0);
    int byte = fgetc(file);
    assert(byte != EOF);
    assert(fseek(file, offset, SEEK_SET) == 0);
    assert(fputc(byte ^ 1, file) != EOF);
    assert(fclose(file) == 0);
}

int test_disk_image(void)
{
    static const char path[] = "build/test_cvm_boot_disk.img";
    (void)remove(path);

    size_t kernel_size;
    uint8_t *kernel = make_test_kernel(&kernel_size);
    char error[192];
    assert(cvm_disk_image_create(path,
                                 CVM_DISK_MIN_SIZE,
                                 kernel,
                                 kernel_size,
                                 kernel,
                                 kernel_size,
                                 CVM_DISK_CREATE_REPRODUCIBLE,
                                 error,
                                 sizeof(error)) == CVM_DISK_OK);

    CvmDiskImageInfo info;
    assert(cvm_disk_image_inspect(path,
                                  &info,
                                  error,
                                  sizeof(error)) == CVM_DISK_OK);
    assert(info.disk_size == CVM_DISK_MIN_SIZE);
    assert(info.partition_start_lba == CVM_BOOT_PARTITION_START_LBA);
    assert(info.sectors_per_cluster == 1);
    assert(info.cluster_count >= 65525);
    assert(info.bootloader_first_cluster == 4);
    assert(info.bootloader_size == kernel_size);
    assert(info.kernel_first_cluster == 5);
    assert(info.kernel_size == kernel_size);
    assert((info.disk_guid[7] & 0xF0) == 0x50);
    assert((info.disk_guid[8] & 0xC0) == 0x80);

    assert(cvm_disk_image_create(path,
                                 CVM_DISK_MIN_SIZE,
                                 kernel,
                                 kernel_size,
                                 kernel,
                                 kernel_size,
                                 CVM_DISK_CREATE_REPRODUCIBLE,
                                 error,
                                 sizeof(error)) ==
           CVM_DISK_ALREADY_EXISTS);

    flip_byte(path, 512);
    assert(cvm_disk_image_inspect(path,
                                  NULL,
                                  error,
                                  sizeof(error)) == CVM_DISK_BAD_GPT);
    flip_byte(path, 512);
    assert(cvm_disk_image_inspect(path,
                                  &info,
                                  error,
                                  sizeof(error)) == CVM_DISK_OK);

    uint64_t second_fat_lba = info.partition_start_lba + 32 +
                              info.fat_sectors;
    assert(second_fat_lba <= (uint64_t)LONG_MAX / 512);
    flip_byte(path, (long)(second_fat_lba * 512));
    assert(cvm_disk_image_inspect(path,
                                  NULL,
                                  error,
                                  sizeof(error)) == CVM_DISK_BAD_FAT32);
    flip_byte(path, (long)(second_fat_lba * 512));

    uint64_t first_data_lba = info.partition_start_lba + 32 +
                              (uint64_t)2 * info.fat_sectors;
    uint64_t bootloader_lba = first_data_lba +
                              (uint64_t)(info.bootloader_first_cluster - 2) *
                                  info.sectors_per_cluster;
    assert(bootloader_lba <= (uint64_t)LONG_MAX / 512);
    flip_byte(path, (long)(bootloader_lba * 512));
    assert(cvm_disk_image_inspect(path,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_DISK_BAD_BOOTLOADER);
    flip_byte(path, (long)(bootloader_lba * 512));

    uint64_t kernel_lba = first_data_lba +
                          (uint64_t)(info.kernel_first_cluster - 2) *
                              info.sectors_per_cluster;
    assert(kernel_lba <= (uint64_t)LONG_MAX / 512);
    flip_byte(path, (long)(kernel_lba * 512));
    assert(cvm_disk_image_inspect(path,
                                  NULL,
                                  error,
                                  sizeof(error)) == CVM_DISK_BAD_KERNEL);
    assert(remove(path) == 0);

    assert(cvm_disk_image_create(path,
                                 CVM_DISK_MIN_SIZE,
                                 kernel,
                                 kernel_size,
                                 kernel,
                                 kernel_size,
                                 0,
                                 error,
                                 sizeof(error)) == CVM_DISK_OK);
    assert(cvm_disk_image_inspect(path,
                                  &info,
                                  error,
                                  sizeof(error)) == CVM_DISK_OK);
    assert((info.disk_guid[7] & 0xF0) == 0x40);
    assert((info.disk_guid[8] & 0xC0) == 0x80);
    assert((info.partition_guid[7] & 0xF0) == 0x40);
    assert((info.partition_guid[8] & 0xC0) == 0x80);
    assert(remove(path) == 0);

    kernel[0] ^= 1;
    assert(cvm_disk_image_create(path,
                                 CVM_DISK_MIN_SIZE,
                                 kernel,
                                 kernel_size,
                                 kernel,
                                 kernel_size,
                                 CVM_DISK_CREATE_REPRODUCIBLE,
                                 error,
                                 sizeof(error)) == CVM_DISK_BAD_BOOTLOADER);
    FILE *missing = fopen(path, "rb");
    assert(missing == NULL);
    free(kernel);
    return 0;
}
