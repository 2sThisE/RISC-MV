#ifndef DISK_IMAGE_H
#define DISK_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define CVM_DISK_SECTOR_SIZE UINT64_C(512)
#define CVM_DISK_MIN_SIZE (UINT64_C(64) * 1024 * 1024)
#define CVM_DISK_MAX_SIZE (UINT64_C(4) * 1024 * 1024 * 1024)
#define CVM_GPT_PARTITION_ENTRY_COUNT UINT32_C(128)
#define CVM_GPT_PARTITION_ENTRY_SIZE UINT32_C(128)
#define CVM_BOOT_PARTITION_START_LBA UINT64_C(2048)
#define CVM_BOOT_PARTITION_TYPE_GUID_STRING \
    "9f7c3a21-6d52-4bc8-a3e1-43564d424f4f"

#define CVM_DISK_CREATE_REPRODUCIBLE (UINT32_C(1) << 0)

extern const uint8_t CVM_BOOT_PARTITION_TYPE_GUID_BYTES[16];

typedef enum {
    CVM_DISK_OK = 0,
    CVM_DISK_INVALID_ARGUMENT,
    CVM_DISK_ALREADY_EXISTS,
    CVM_DISK_BAD_SIZE,
    CVM_DISK_BAD_BOOTLOADER,
    CVM_DISK_BAD_KERNEL,
    CVM_DISK_BAD_GPT,
    CVM_DISK_BAD_FAT32,
    CVM_DISK_KERNEL_NOT_FOUND,
    CVM_DISK_OUT_OF_MEMORY,
    CVM_DISK_IO_ERROR
} CvmDiskStatus;

typedef struct {
    uint8_t disk_guid[16];
    uint8_t partition_guid[16];
    uint64_t disk_size;
    uint64_t total_sectors;
    uint64_t partition_start_lba;
    uint64_t partition_sectors;
    uint32_t fat_sectors;
    uint32_t sectors_per_cluster;
    uint32_t cluster_count;
    uint32_t bootloader_first_cluster;
    uint64_t bootloader_size;
    uint32_t kernel_first_cluster;
    uint64_t kernel_size;
} CvmDiskImageInfo;

CvmDiskStatus cvm_disk_image_create(const char *path,
                                     uint64_t disk_size,
                                     const uint8_t *bootloader_image,
                                     size_t bootloader_size,
                                     const uint8_t *kernel_image,
                                     size_t kernel_size,
                                     uint32_t create_flags,
                                     char *error,
                                     size_t error_size);

CvmDiskStatus cvm_disk_image_inspect(const char *path,
                                      CvmDiskImageInfo *info,
                                      char *error,
                                      size_t error_size);

const char *cvm_disk_status_name(CvmDiskStatus status);

#endif
