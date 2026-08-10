#include "disk_image.h"

#include "boot_format.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define GPT_HEADER_SIZE UINT32_C(92)
#define GPT_PRIMARY_HEADER_LBA UINT64_C(1)
#define GPT_PRIMARY_ENTRIES_LBA UINT64_C(2)
#define GPT_ENTRY_ARRAY_SECTORS UINT64_C(32)
#define FAT_RESERVED_SECTORS UINT32_C(32)
#define FAT_COUNT UINT32_C(2)
#define FAT_ROOT_CLUSTER UINT32_C(2)
#define FAT_BOOT_CLUSTER UINT32_C(3)
#define FAT_BOOTLOADER_CLUSTER UINT32_C(4)
#define FAT32_MIN_CLUSTERS UINT32_C(65525)
#define FAT32_MAX_CLUSTERS UINT32_C(0x0FFFFFF5)
#define FAT32_END_OF_CHAIN UINT32_C(0x0FFFFFFF)

const uint8_t CVM_BOOT_PARTITION_TYPE_GUID_BYTES[16] = {
    0x21, 0x3A, 0x7C, 0x9F, 0x52, 0x6D, 0xC8, 0x4B,
    0xA3, 0xE1, 0x43, 0x56, 0x4D, 0x42, 0x4F, 0x4F
};

typedef struct {
    uint64_t partition_start;
    uint32_t total_sectors;
    uint32_t sectors_per_cluster;
    uint32_t fat_sectors;
    uint32_t cluster_count;
    uint64_t first_data_lba;
    uint32_t bootloader_clusters;
    uint32_t kernel_first_cluster;
    uint32_t kernel_clusters;
} FatLayout;

static uint16_t read_u16_le(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] |
                      ((uint16_t)input[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *input)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= (uint64_t)input[i] << (i * 8);
    }
    return value;
}

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) {
        output[i] = (uint8_t)(value >> (i * 8));
    }
}

static void write_u64_le(uint8_t *output, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        output[i] = (uint8_t)(value >> (i * 8));
    }
}

static CvmDiskStatus disk_fail(CvmDiskStatus status,
                               char *error,
                               size_t error_size,
                               const char *message)
{
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return status;
}

static int file_seek(FILE *file, uint64_t offset)
{
#if defined(_WIN32)
    return _fseeki64(file, (long long)offset, SEEK_SET) == 0;
#else
    if (offset > (uint64_t)LONG_MAX) {
        return 0;
    }
    return fseek(file, (long)offset, SEEK_SET) == 0;
#endif
}

static int file_size(FILE *file, uint64_t *size)
{
#if defined(_WIN32)
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        return 0;
    }
    long long measured = _ftelli64(file);
    if (measured < 0 || _fseeki64(file, 0, SEEK_SET) != 0) {
        return 0;
    }
    *size = (uint64_t)measured;
#else
    if (fseek(file, 0, SEEK_END) != 0) {
        return 0;
    }
    long measured = ftell(file);
    if (measured < 0 || fseek(file, 0, SEEK_SET) != 0) {
        return 0;
    }
    *size = (uint64_t)(unsigned long)measured;
#endif
    return 1;
}

static int write_at(FILE *file,
                    uint64_t offset,
                    const void *data,
                    size_t size)
{
    return file_seek(file, offset) && fwrite(data, 1, size, file) == size;
}

static int read_at(FILE *file, uint64_t offset, void *data, size_t size)
{
    return file_seek(file, offset) && fread(data, 1, size, file) == size;
}

static int write_sector(FILE *file, uint64_t lba, const uint8_t sector[512])
{
    if (lba > UINT64_MAX / CVM_DISK_SECTOR_SIZE) {
        return 0;
    }
    return write_at(file, lba * CVM_DISK_SECTOR_SIZE, sector, 512);
}

static int read_sector(FILE *file, uint64_t lba, uint8_t sector[512])
{
    if (lba > UINT64_MAX / CVM_DISK_SECTOR_SIZE) {
        return 0;
    }
    return read_at(file, lba * CVM_DISK_SECTOR_SIZE, sector, 512);
}

static int path_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static uint64_t fnv1a64(const uint8_t *data, size_t size, uint64_t seed)
{
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void make_reproducible_guid(uint8_t output[16],
                                   const uint8_t *kernel,
                                   size_t kernel_size,
                                   uint64_t disk_size,
                                   uint64_t discriminator)
{
    uint64_t first = fnv1a64(kernel,
                             kernel_size,
                             UINT64_C(14695981039346656037) ^
                                 discriminator);
    uint8_t metadata[16];
    write_u64_le(metadata, disk_size);
    write_u64_le(metadata + 8, discriminator);
    uint64_t second = fnv1a64(metadata,
                              sizeof(metadata),
                              first ^ UINT64_C(0x9E3779B97F4A7C15));
    write_u64_le(output, first);
    write_u64_le(output + 8, second);
    output[7] = (uint8_t)((output[7] & 0x0F) | 0x50);
    output[8] = (uint8_t)((output[8] & 0x3F) | 0x80);
}

static int random_bytes(uint8_t *output, size_t size)
{
#if defined(_WIN32)
    if (size > UINT32_MAX) {
        return 0;
    }
    return BCryptGenRandom(NULL,
                           output,
                           (ULONG)size,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    int descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) {
        return 0;
    }
    size_t complete = 0;
    while (complete < size) {
        ssize_t count = read(descriptor, output + complete, size - complete);
        if (count <= 0) {
            (void)close(descriptor);
            return 0;
        }
        complete += (size_t)count;
    }
    return close(descriptor) == 0;
#endif
}

static int make_random_guid(uint8_t output[16])
{
    if (!random_bytes(output, 16)) {
        return 0;
    }
    output[7] = (uint8_t)((output[7] & 0x0F) | 0x40);
    output[8] = (uint8_t)((output[8] & 0x3F) | 0x80);
    return 1;
}

static void make_protective_mbr(uint8_t sector[512], uint64_t total_sectors)
{
    memset(sector, 0, 512);
    uint8_t *entry = sector + 446;
    entry[0] = 0x00;
    entry[1] = 0x00;
    entry[2] = 0x02;
    entry[3] = 0x00;
    entry[4] = 0xEE;
    entry[5] = 0xFF;
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    write_u32_le(entry + 8, 1);
    uint64_t protected_sectors = total_sectors - 1;
    write_u32_le(entry + 12,
                 protected_sectors > UINT32_MAX
                     ? UINT32_MAX
                     : (uint32_t)protected_sectors);
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static void make_partition_entries(uint8_t entries[16384],
                                   const uint8_t partition_guid[16],
                                   uint64_t first_lba,
                                   uint64_t last_lba)
{
    memset(entries, 0, 16384);
    memcpy(entries, CVM_BOOT_PARTITION_TYPE_GUID_BYTES, 16);
    memcpy(entries + 16, partition_guid, 16);
    write_u64_le(entries + 32, first_lba);
    write_u64_le(entries + 40, last_lba);
    static const char name[] = "CVM Boot";
    for (size_t i = 0; i < sizeof(name) - 1; ++i) {
        write_u16_le(entries + 56 + i * 2, (uint16_t)(uint8_t)name[i]);
    }
}

static void make_gpt_header(uint8_t sector[512],
                            uint64_t current_lba,
                            uint64_t backup_lba,
                            uint64_t entries_lba,
                            uint64_t first_usable,
                            uint64_t last_usable,
                            const uint8_t disk_guid[16],
                            uint32_t entries_crc)
{
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    write_u32_le(sector + 8, UINT32_C(0x00010000));
    write_u32_le(sector + 12, GPT_HEADER_SIZE);
    write_u64_le(sector + 24, current_lba);
    write_u64_le(sector + 32, backup_lba);
    write_u64_le(sector + 40, first_usable);
    write_u64_le(sector + 48, last_usable);
    memcpy(sector + 56, disk_guid, 16);
    write_u64_le(sector + 72, entries_lba);
    write_u32_le(sector + 80, CVM_GPT_PARTITION_ENTRY_COUNT);
    write_u32_le(sector + 84, CVM_GPT_PARTITION_ENTRY_SIZE);
    write_u32_le(sector + 88, entries_crc);
    write_u32_le(sector + 16, cvm_crc32(sector, GPT_HEADER_SIZE));
}

static uint32_t choose_sectors_per_cluster(uint32_t total_sectors)
{
    return total_sectors < UINT32_C(532480) ? 1 : 8;
}

static int calculate_fat_layout(uint64_t partition_start,
                                uint64_t partition_sectors,
                                size_t bootloader_size,
                                size_t kernel_size,
                                FatLayout *layout)
{
    if (partition_sectors > UINT32_MAX || bootloader_size > UINT32_MAX ||
        kernel_size > UINT32_MAX) {
        return 0;
    }
    uint32_t total = (uint32_t)partition_sectors;
    uint32_t sectors_per_cluster = choose_sectors_per_cluster(total);
    uint32_t fat_sectors = 1;
    uint32_t cluster_count = 0;
    for (unsigned int iteration = 0; iteration < 32; ++iteration) {
        uint64_t overhead = FAT_RESERVED_SECTORS +
                            (uint64_t)FAT_COUNT * fat_sectors;
        if (overhead >= total) {
            return 0;
        }
        cluster_count = (uint32_t)((total - overhead) /
                                   sectors_per_cluster);
        uint64_t fat_bytes = ((uint64_t)cluster_count + 2) * 4;
        uint32_t needed = (uint32_t)((fat_bytes + 511) / 512);
        /* A FAT that is already large enough is valid. Replacing it with the
           smaller estimate can oscillate by one sector at the boundary. */
        if (needed <= fat_sectors) {
            break;
        }
        fat_sectors = needed;
        if (iteration == 31) {
            return 0;
        }
    }
    if (cluster_count < FAT32_MIN_CLUSTERS ||
        cluster_count > FAT32_MAX_CLUSTERS) {
        return 0;
    }

    uint64_t cluster_bytes = (uint64_t)sectors_per_cluster * 512;
    uint64_t bootloader_clusters64 =
        ((uint64_t)bootloader_size + cluster_bytes - 1) / cluster_bytes;
    uint64_t kernel_clusters64 =
        ((uint64_t)kernel_size + cluster_bytes - 1) / cluster_bytes;
    if (bootloader_clusters64 == 0 ||
        bootloader_clusters64 > UINT32_MAX || kernel_clusters64 == 0 ||
        kernel_clusters64 > UINT32_MAX) {
        return 0;
    }
    uint32_t bootloader_clusters = (uint32_t)bootloader_clusters64;
    uint32_t kernel_clusters = (uint32_t)kernel_clusters64;
    uint64_t kernel_first64 = FAT_BOOTLOADER_CLUSTER +
                              bootloader_clusters64;
    if (kernel_first64 > UINT32_MAX ||
        kernel_first64 + kernel_clusters64 - 1 >
        (uint64_t)cluster_count + 1) {
        return 0;
    }

    layout->partition_start = partition_start;
    layout->total_sectors = total;
    layout->sectors_per_cluster = sectors_per_cluster;
    layout->fat_sectors = fat_sectors;
    layout->cluster_count = cluster_count;
    layout->first_data_lba = partition_start + FAT_RESERVED_SECTORS +
                             (uint64_t)FAT_COUNT * fat_sectors;
    layout->bootloader_clusters = bootloader_clusters;
    layout->kernel_first_cluster = (uint32_t)kernel_first64;
    layout->kernel_clusters = kernel_clusters;
    return 1;
}

static void make_fat_boot_sector(uint8_t sector[512],
                                 const FatLayout *layout,
                                 uint32_t volume_id)
{
    memset(sector, 0, 512);
    sector[0] = 0xEB;
    sector[1] = 0x58;
    sector[2] = 0x90;
    memcpy(sector + 3, "CVMFAT32", 8);
    write_u16_le(sector + 11, 512);
    sector[13] = (uint8_t)layout->sectors_per_cluster;
    write_u16_le(sector + 14, FAT_RESERVED_SECTORS);
    sector[16] = FAT_COUNT;
    write_u16_le(sector + 17, 0);
    write_u16_le(sector + 19, 0);
    sector[21] = 0xF8;
    write_u16_le(sector + 22, 0);
    write_u16_le(sector + 24, 63);
    write_u16_le(sector + 26, 255);
    write_u32_le(sector + 28, (uint32_t)layout->partition_start);
    write_u32_le(sector + 32, layout->total_sectors);
    write_u32_le(sector + 36, layout->fat_sectors);
    write_u16_le(sector + 40, 0);
    write_u16_le(sector + 42, 0);
    write_u32_le(sector + 44, FAT_ROOT_CLUSTER);
    write_u16_le(sector + 48, 1);
    write_u16_le(sector + 50, 6);
    sector[64] = 0x80;
    sector[66] = 0x29;
    write_u32_le(sector + 67, volume_id);
    memcpy(sector + 71, "CVM BOOT   ", 11);
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static void make_fsinfo_sector(uint8_t sector[512],
                               uint32_t free_clusters,
                               uint32_t next_free)
{
    memset(sector, 0, 512);
    write_u32_le(sector + 0, UINT32_C(0x41615252));
    write_u32_le(sector + 484, UINT32_C(0x61417272));
    write_u32_le(sector + 488, free_clusters);
    write_u32_le(sector + 492, next_free);
    write_u32_le(sector + 508, UINT32_C(0xAA550000));
}

static void set_fat_entry(uint8_t *fat, uint32_t cluster, uint32_t value)
{
    write_u32_le(fat + (size_t)cluster * 4, value);
}

static void make_directory_entry(uint8_t entry[32],
                                 const char name[11],
                                 uint8_t attributes,
                                 uint32_t cluster,
                                 uint32_t size)
{
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = attributes;
    const uint16_t date = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
    write_u16_le(entry + 14, 0);
    write_u16_le(entry + 16, date);
    write_u16_le(entry + 18, date);
    write_u16_le(entry + 20, (uint16_t)(cluster >> 16));
    write_u16_le(entry + 22, 0);
    write_u16_le(entry + 24, date);
    write_u16_le(entry + 26, (uint16_t)cluster);
    write_u32_le(entry + 28, size);
}

static uint64_t cluster_lba(const FatLayout *layout, uint32_t cluster)
{
    return layout->first_data_lba +
           (uint64_t)(cluster - 2) * layout->sectors_per_cluster;
}

static int write_fat32(FILE *file,
                       const FatLayout *layout,
                       const uint8_t *bootloader,
                       size_t bootloader_size,
                       const uint8_t *kernel,
                       size_t kernel_size,
                       uint32_t volume_id)
{
    uint8_t sector[512];
    make_fat_boot_sector(sector, layout, volume_id);
    if (!write_sector(file, layout->partition_start, sector) ||
        !write_sector(file, layout->partition_start + 6, sector)) {
        return 0;
    }

    uint32_t used_clusters = 2 + layout->bootloader_clusters +
                             layout->kernel_clusters;
    uint32_t free_clusters = layout->cluster_count - used_clusters;
    uint32_t next_free = layout->kernel_first_cluster +
                         layout->kernel_clusters;
    make_fsinfo_sector(sector, free_clusters, next_free);
    if (!write_sector(file, layout->partition_start + 1, sector) ||
        !write_sector(file, layout->partition_start + 7, sector)) {
        return 0;
    }

    size_t fat_bytes = (size_t)layout->fat_sectors * 512;
    uint8_t *fat = calloc(1, fat_bytes);
    if (fat == NULL) {
        return 0;
    }
    set_fat_entry(fat, 0, UINT32_C(0x0FFFFFF8));
    set_fat_entry(fat, 1, FAT32_END_OF_CHAIN);
    set_fat_entry(fat, FAT_ROOT_CLUSTER, FAT32_END_OF_CHAIN);
    set_fat_entry(fat, FAT_BOOT_CLUSTER, FAT32_END_OF_CHAIN);
    for (uint32_t i = 0; i < layout->bootloader_clusters; ++i) {
        uint32_t cluster = FAT_BOOTLOADER_CLUSTER + i;
        uint32_t next = i + 1 == layout->bootloader_clusters
                            ? FAT32_END_OF_CHAIN
                            : cluster + 1;
        set_fat_entry(fat, cluster, next);
    }
    for (uint32_t i = 0; i < layout->kernel_clusters; ++i) {
        uint32_t cluster = layout->kernel_first_cluster + i;
        uint32_t next = i + 1 == layout->kernel_clusters
                            ? FAT32_END_OF_CHAIN
                            : cluster + 1;
        set_fat_entry(fat, cluster, next);
    }
    uint64_t first_fat_lba = layout->partition_start + FAT_RESERVED_SECTORS;
    int okay = write_at(file,
                        first_fat_lba * 512,
                        fat,
                        fat_bytes) &&
               write_at(file,
                        (first_fat_lba + layout->fat_sectors) * 512,
                        fat,
                        fat_bytes);
    free(fat);
    if (!okay) {
        return 0;
    }

    size_t cluster_bytes = (size_t)layout->sectors_per_cluster * 512;
    uint8_t *directory = calloc(1, cluster_bytes);
    if (directory == NULL) {
        return 0;
    }
    make_directory_entry(directory,
                         "CVM BOOT   ",
                         0x08,
                         0,
                         0);
    make_directory_entry(directory + 32,
                         "BOOT       ",
                         0x10,
                         FAT_BOOT_CLUSTER,
                         0);
    okay = write_at(file,
                    cluster_lba(layout, FAT_ROOT_CLUSTER) * 512,
                    directory,
                    cluster_bytes);
    if (okay) {
        memset(directory, 0, cluster_bytes);
        make_directory_entry(directory,
                             ".          ",
                             0x10,
                             FAT_BOOT_CLUSTER,
                             0);
        make_directory_entry(directory + 32,
                             "..         ",
                             0x10,
                             FAT_ROOT_CLUSTER,
                             0);
        make_directory_entry(directory + 64,
                             "BOOT    CVM",
                             0x20,
                             FAT_BOOTLOADER_CLUSTER,
                             (uint32_t)bootloader_size);
        make_directory_entry(directory + 96,
                             "KERNEL  CVM",
                             0x20,
                             layout->kernel_first_cluster,
                             (uint32_t)kernel_size);
        okay = write_at(file,
                        cluster_lba(layout, FAT_BOOT_CLUSTER) * 512,
                        directory,
                        cluster_bytes);
    }
    free(directory);
    if (!okay) {
        return 0;
    }

    return write_at(file,
                    cluster_lba(layout, FAT_BOOTLOADER_CLUSTER) * 512,
                    bootloader,
                    bootloader_size) &&
           write_at(file,
                    cluster_lba(layout, layout->kernel_first_cluster) * 512,
                    kernel,
                    kernel_size);
}

static CvmDiskStatus create_disk_contents(FILE *file,
                                          uint64_t disk_size,
                                          const uint8_t *bootloader,
                                          size_t bootloader_size,
                                          const uint8_t *kernel,
                                          size_t kernel_size,
                                          uint32_t create_flags,
                                          char *error,
                                          size_t error_size)
{
    uint64_t total_sectors = disk_size / 512;
    uint64_t backup_header_lba = total_sectors - 1;
    uint64_t backup_entries_lba = total_sectors - 33;
    uint64_t first_usable = 34;
    uint64_t last_usable = total_sectors - 34;
    if (CVM_BOOT_PARTITION_START_LBA > last_usable) {
        return disk_fail(CVM_DISK_BAD_SIZE,
                         error, error_size, "disk is too small for GPT layout");
    }
    uint64_t partition_sectors = last_usable -
                                 CVM_BOOT_PARTITION_START_LBA + 1;

    FatLayout fat_layout;
    if (!calculate_fat_layout(CVM_BOOT_PARTITION_START_LBA,
                              partition_sectors,
                              bootloader_size,
                              kernel_size,
                              &fat_layout)) {
        return disk_fail(CVM_DISK_BAD_SIZE,
                         error, error_size,
                         "disk cannot hold a valid FAT32 boot partition");
    }

    uint8_t disk_guid[16];
    uint8_t partition_guid[16];
    if ((create_flags & CVM_DISK_CREATE_REPRODUCIBLE) != 0) {
        make_reproducible_guid(disk_guid,
                               bootloader,
                               bootloader_size,
                               disk_size,
                               UINT64_C(0x4449534B));
        make_reproducible_guid(partition_guid,
                               kernel,
                               kernel_size,
                               disk_size,
                               UINT64_C(0x50415254));
    } else if (!make_random_guid(disk_guid) ||
               !make_random_guid(partition_guid)) {
        return disk_fail(CVM_DISK_IO_ERROR,
                         error, error_size,
                         "cannot obtain operating-system random bytes");
    }

    uint8_t mbr[512];
    make_protective_mbr(mbr, total_sectors);
    uint8_t entries[16384];
    make_partition_entries(entries,
                           partition_guid,
                           CVM_BOOT_PARTITION_START_LBA,
                           last_usable);
    uint32_t entries_crc = cvm_crc32(entries, sizeof(entries));
    uint8_t primary_header[512];
    uint8_t backup_header[512];
    make_gpt_header(primary_header,
                    GPT_PRIMARY_HEADER_LBA,
                    backup_header_lba,
                    GPT_PRIMARY_ENTRIES_LBA,
                    first_usable,
                    last_usable,
                    disk_guid,
                    entries_crc);
    make_gpt_header(backup_header,
                    backup_header_lba,
                    GPT_PRIMARY_HEADER_LBA,
                    backup_entries_lba,
                    first_usable,
                    last_usable,
                    disk_guid,
                    entries_crc);

    if (!write_sector(file, 0, mbr) ||
        !write_sector(file, GPT_PRIMARY_HEADER_LBA, primary_header) ||
        !write_at(file, GPT_PRIMARY_ENTRIES_LBA * 512,
                  entries, sizeof(entries)) ||
        !write_at(file, backup_entries_lba * 512,
                  entries, sizeof(entries)) ||
        !write_sector(file, backup_header_lba, backup_header)) {
        return disk_fail(CVM_DISK_IO_ERROR,
                         error, error_size, "cannot write GPT metadata");
    }

    uint32_t volume_id = cvm_crc32(bootloader, bootloader_size) ^
                         cvm_crc32(kernel, kernel_size) ^
                         (uint32_t)total_sectors;
    if (!write_fat32(file,
                     &fat_layout,
                     bootloader,
                     bootloader_size,
                     kernel,
                     kernel_size,
                     volume_id)) {
        return disk_fail(CVM_DISK_IO_ERROR,
                         error, error_size, "cannot write FAT32 volume");
    }
    return CVM_DISK_OK;
}

static CvmDiskStatus validate_gpt_header(const uint8_t sector[512],
                                         uint64_t expected_current,
                                         uint64_t expected_backup,
                                         uint64_t total_sectors,
                                         char *error,
                                         size_t error_size)
{
    if (memcmp(sector, "EFI PART", 8) != 0 ||
        read_u32_le(sector + 8) != UINT32_C(0x00010000)) {
        return disk_fail(CVM_DISK_BAD_GPT,
                         error, error_size, "invalid GPT signature or revision");
    }
    uint32_t header_size = read_u32_le(sector + 12);
    if (header_size < GPT_HEADER_SIZE || header_size > 512 ||
        read_u64_le(sector + 24) != expected_current ||
        read_u64_le(sector + 32) != expected_backup ||
        expected_current >= total_sectors) {
        return disk_fail(CVM_DISK_BAD_GPT,
                         error, error_size, "invalid GPT header geometry");
    }
    uint8_t copy[512];
    memcpy(copy, sector, 512);
    uint32_t stored_crc = read_u32_le(copy + 16);
    write_u32_le(copy + 16, 0);
    if (stored_crc != cvm_crc32(copy, header_size)) {
        return disk_fail(CVM_DISK_BAD_GPT,
                         error, error_size, "GPT header CRC32 mismatch");
    }
    return CVM_DISK_OK;
}

static int find_directory_entry(const uint8_t *directory,
                                size_t directory_size,
                                const char name[11],
                                uint8_t required_attribute,
                                uint32_t *cluster,
                                uint32_t *size)
{
    for (size_t offset = 0; offset + 32 <= directory_size; offset += 32) {
        const uint8_t *entry = directory + offset;
        if (entry[0] == 0x00) {
            break;
        }
        if (entry[0] == 0xE5 || entry[11] == 0x0F ||
            memcmp(entry, name, 11) != 0 ||
            (entry[11] & required_attribute) == 0) {
            continue;
        }
        *cluster = ((uint32_t)read_u16_le(entry + 20) << 16) |
                   read_u16_le(entry + 26);
        *size = read_u32_le(entry + 28);
        return 1;
    }
    return 0;
}

static int read_cluster(FILE *file,
                        const FatLayout *layout,
                        uint32_t cluster,
                        uint8_t *data,
                        size_t size)
{
    if (cluster < 2 || cluster > layout->cluster_count + 1 ||
        size != (size_t)layout->sectors_per_cluster * 512) {
        return 0;
    }
    return read_at(file, cluster_lba(layout, cluster) * 512, data, size);
}

static CvmDiskStatus validate_fat_image(FILE *file,
                                        const FatLayout *layout,
                                        const uint8_t *fat,
                                        uint32_t first_cluster,
                                        uint32_t image_size,
                                        CvmDiskStatus invalid_status,
                                        const char *kind,
                                        char *error,
                                        size_t error_size)
{
    size_t cluster_bytes = (size_t)layout->sectors_per_cluster * 512;
    uint8_t *image = malloc(image_size);
    uint8_t *cluster_data = malloc(cluster_bytes);
    if (image == NULL || cluster_data == NULL) {
        free(image);
        free(cluster_data);
        return disk_fail(CVM_DISK_OUT_OF_MEMORY,
                         error, error_size, "cannot allocate image buffer");
    }

    CvmDiskStatus status = CVM_DISK_OK;
    size_t copied = 0;
    uint32_t current = first_cluster;
    uint32_t visited = 0;
    while (copied < image_size) {
        if (++visited > layout->cluster_count ||
            !read_cluster(file,
                          layout,
                          current,
                          cluster_data,
                          cluster_bytes)) {
            status = disk_fail(CVM_DISK_BAD_FAT32,
                               error, error_size,
                               "invalid executable cluster chain");
            break;
        }
        size_t amount = image_size - copied;
        if (amount > cluster_bytes) {
            amount = cluster_bytes;
        }
        memcpy(image + copied, cluster_data, amount);
        copied += amount;
        uint32_t next = read_u32_le(fat + (size_t)current * 4) &
                        UINT32_C(0x0FFFFFFF);
        if (copied < image_size) {
            if (next < 2 || next > layout->cluster_count + 1 ||
                next >= UINT32_C(0x0FFFFFF8)) {
                status = disk_fail(CVM_DISK_BAD_FAT32,
                                   error, error_size,
                                   "executable cluster chain ends early");
                break;
            }
            current = next;
        } else if (next < UINT32_C(0x0FFFFFF8)) {
            status = disk_fail(CVM_DISK_BAD_FAT32,
                               error, error_size,
                               "executable cluster chain has extra clusters");
        }
    }
    free(cluster_data);
    if (status == CVM_DISK_OK) {
        char image_error[160];
        if (cvm_kernel_image_validate(image,
                                      image_size,
                                      NULL,
                                      image_error,
                                      sizeof(image_error)) !=
            CVM_BOOT_FORMAT_OK) {
            status = disk_fail(invalid_status,
                               error, error_size, image_error);
        }
    }
    free(image);
    (void)kind;
    return status;
}

CvmDiskStatus cvm_disk_image_create(const char *path,
                                     uint64_t disk_size,
                                     const uint8_t *bootloader_image,
                                     size_t bootloader_size,
                                     const uint8_t *kernel_image,
                                     size_t kernel_size,
                                     uint32_t create_flags,
                                     char *error,
                                     size_t error_size)
{
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    if (path == NULL || *path == '\0' || bootloader_image == NULL ||
        bootloader_size == 0 || kernel_image == NULL || kernel_size == 0 ||
        (create_flags & ~CVM_DISK_CREATE_REPRODUCIBLE) != 0) {
        return disk_fail(CVM_DISK_INVALID_ARGUMENT,
                         error, error_size, "invalid create arguments");
    }
    if (disk_size < CVM_DISK_MIN_SIZE || disk_size > CVM_DISK_MAX_SIZE ||
        disk_size % CVM_DISK_SECTOR_SIZE != 0) {
        return disk_fail(CVM_DISK_BAD_SIZE,
                         error, error_size,
                         "disk size must be a 512-byte multiple from 64 MiB to 4 GiB");
    }
    char kernel_error[160];
    if (cvm_kernel_image_validate(bootloader_image,
                                  bootloader_size,
                                  NULL,
                                  kernel_error,
                                  sizeof(kernel_error)) != CVM_BOOT_FORMAT_OK) {
        return disk_fail(CVM_DISK_BAD_BOOTLOADER,
                         error, error_size, kernel_error);
    }
    if (cvm_kernel_image_validate(kernel_image,
                                  kernel_size,
                                  NULL,
                                  kernel_error,
                                  sizeof(kernel_error)) != CVM_BOOT_FORMAT_OK) {
        return disk_fail(CVM_DISK_BAD_KERNEL,
                         error, error_size, kernel_error);
    }
    if (path_exists(path)) {
        return disk_fail(CVM_DISK_ALREADY_EXISTS,
                         error, error_size, "output image already exists");
    }

    FILE *file = fopen(path, "wb+");
    if (file == NULL) {
        return disk_fail(CVM_DISK_IO_ERROR,
                         error, error_size, "cannot create output image");
    }
    int allocated = file_seek(file, disk_size - 1) && fputc(0, file) != EOF;
    CvmDiskStatus status = allocated
                               ? create_disk_contents(file,
                                                      disk_size,
                                                      bootloader_image,
                                                      bootloader_size,
                                                      kernel_image,
                                                      kernel_size,
                                                      create_flags,
                                                      error,
                                                      error_size)
                               : CVM_DISK_IO_ERROR;
    if (!allocated && error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", "cannot size output image");
    }
    if (status == CVM_DISK_OK && fflush(file) != 0) {
        status = disk_fail(CVM_DISK_IO_ERROR,
                           error, error_size, "cannot flush output image");
    }
    if (fclose(file) != 0 && status == CVM_DISK_OK) {
        status = disk_fail(CVM_DISK_IO_ERROR,
                           error, error_size, "cannot close output image");
    }
    if (status == CVM_DISK_OK) {
        status = cvm_disk_image_inspect(path, NULL, error, error_size);
    }
    if (status != CVM_DISK_OK) {
        (void)remove(path);
    }
    return status;
}

CvmDiskStatus cvm_disk_image_inspect(const char *path,
                                      CvmDiskImageInfo *result,
                                      char *error,
                                      size_t error_size)
{
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    if (path == NULL || *path == '\0') {
        return disk_fail(CVM_DISK_INVALID_ARGUMENT,
                         error, error_size, "invalid image path");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return disk_fail(CVM_DISK_IO_ERROR,
                         error, error_size, "cannot open disk image");
    }
    CvmDiskStatus status = CVM_DISK_OK;
    uint64_t disk_size = 0;
    if (!file_size(file, &disk_size) || disk_size < CVM_DISK_MIN_SIZE ||
        disk_size % 512 != 0) {
        status = disk_fail(CVM_DISK_BAD_SIZE,
                           error, error_size, "invalid disk image size");
        goto done;
    }
    uint64_t total_sectors = disk_size / 512;

    uint8_t sector[512];
    if (!read_sector(file, 0, sector)) {
        status = disk_fail(CVM_DISK_IO_ERROR,
                           error, error_size, "cannot read protective MBR");
        goto done;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA ||
        sector[446 + 4] != 0xEE || read_u32_le(sector + 446 + 8) != 1) {
        status = disk_fail(CVM_DISK_BAD_GPT,
                           error, error_size, "invalid protective MBR");
        goto done;
    }

    uint8_t primary_header[512];
    uint8_t backup_header[512];
    if (!read_sector(file, 1, primary_header) ||
        !read_sector(file, total_sectors - 1, backup_header)) {
        status = disk_fail(CVM_DISK_IO_ERROR,
                           error, error_size, "cannot read GPT headers");
        goto done;
    }
    status = validate_gpt_header(primary_header,
                                 1,
                                 total_sectors - 1,
                                 total_sectors,
                                 error,
                                 error_size);
    if (status != CVM_DISK_OK) {
        goto done;
    }
    status = validate_gpt_header(backup_header,
                                 total_sectors - 1,
                                 1,
                                 total_sectors,
                                 error,
                                 error_size);
    if (status != CVM_DISK_OK) {
        goto done;
    }
    if (read_u32_le(primary_header + 80) !=
            CVM_GPT_PARTITION_ENTRY_COUNT ||
        read_u32_le(primary_header + 84) != CVM_GPT_PARTITION_ENTRY_SIZE ||
        read_u64_le(primary_header + 72) != GPT_PRIMARY_ENTRIES_LBA ||
        read_u64_le(backup_header + 72) != total_sectors - 33 ||
        memcmp(primary_header + 56, backup_header + 56, 16) != 0) {
        status = disk_fail(CVM_DISK_BAD_GPT,
                           error, error_size, "inconsistent GPT tables");
        goto done;
    }

    uint8_t *entries = malloc(16384);
    uint8_t *backup_entries = malloc(16384);
    if (entries == NULL || backup_entries == NULL) {
        free(entries);
        free(backup_entries);
        status = disk_fail(CVM_DISK_OUT_OF_MEMORY,
                           error, error_size, "cannot allocate GPT table");
        goto done;
    }
    int entries_read = read_at(file, GPT_PRIMARY_ENTRIES_LBA * 512,
                               entries, 16384) &&
                       read_at(file, (total_sectors - 33) * 512,
                               backup_entries, 16384);
    if (!entries_read || memcmp(entries, backup_entries, 16384) != 0 ||
        cvm_crc32(entries, 16384) != read_u32_le(primary_header + 88) ||
        read_u32_le(primary_header + 88) !=
            read_u32_le(backup_header + 88)) {
        free(entries);
        free(backup_entries);
        status = disk_fail(CVM_DISK_BAD_GPT,
                           error, error_size, "GPT entry array is invalid");
        goto done;
    }
    free(backup_entries);

    uint64_t partition_start = 0;
    uint64_t partition_last = 0;
    uint8_t partition_guid[16] = {0};
    for (uint32_t i = 0; i < CVM_GPT_PARTITION_ENTRY_COUNT; ++i) {
        const uint8_t *entry = entries +
                               (size_t)i * CVM_GPT_PARTITION_ENTRY_SIZE;
        if (memcmp(entry, CVM_BOOT_PARTITION_TYPE_GUID_BYTES, 16) == 0) {
            partition_start = read_u64_le(entry + 32);
            partition_last = read_u64_le(entry + 40);
            memcpy(partition_guid, entry + 16, 16);
            break;
        }
    }
    free(entries);
    if (partition_start < read_u64_le(primary_header + 40) ||
        partition_last < partition_start ||
        partition_last > read_u64_le(primary_header + 48)) {
        status = disk_fail(CVM_DISK_BAD_GPT,
                           error, error_size, "CVM boot partition is missing");
        goto done;
    }
    uint64_t partition_sectors = partition_last - partition_start + 1;

    uint8_t boot_sector[512];
    uint8_t backup_boot[512];
    uint8_t fsinfo[512];
    if (!read_sector(file, partition_start, boot_sector) ||
        !read_sector(file, partition_start + 6, backup_boot) ||
        !read_sector(file, partition_start + 1, fsinfo)) {
        status = disk_fail(CVM_DISK_IO_ERROR,
                           error, error_size, "cannot read FAT32 metadata");
        goto done;
    }
    if (memcmp(boot_sector, backup_boot, 512) != 0 ||
        boot_sector[510] != 0x55 || boot_sector[511] != 0xAA ||
        read_u16_le(boot_sector + 11) != 512 ||
        boot_sector[16] != FAT_COUNT ||
        read_u16_le(boot_sector + 14) != FAT_RESERVED_SECTORS ||
        read_u32_le(boot_sector + 44) != FAT_ROOT_CLUSTER ||
        memcmp(boot_sector + 82, "FAT32   ", 8) != 0 ||
        read_u32_le(fsinfo) != UINT32_C(0x41615252) ||
        read_u32_le(fsinfo + 484) != UINT32_C(0x61417272) ||
        read_u32_le(fsinfo + 508) != UINT32_C(0xAA550000)) {
        status = disk_fail(CVM_DISK_BAD_FAT32,
                           error, error_size, "invalid FAT32 boot metadata");
        goto done;
    }

    FatLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.partition_start = partition_start;
    layout.total_sectors = read_u32_le(boot_sector + 32);
    layout.sectors_per_cluster = boot_sector[13];
    layout.fat_sectors = read_u32_le(boot_sector + 36);
    if (layout.total_sectors != partition_sectors ||
        layout.sectors_per_cluster == 0 ||
        (layout.sectors_per_cluster &
         (layout.sectors_per_cluster - 1)) != 0 ||
        layout.fat_sectors == 0) {
        status = disk_fail(CVM_DISK_BAD_FAT32,
                           error, error_size, "invalid FAT32 geometry");
        goto done;
    }
    uint64_t overhead = FAT_RESERVED_SECTORS +
                        (uint64_t)FAT_COUNT * layout.fat_sectors;
    if (overhead >= layout.total_sectors) {
        status = disk_fail(CVM_DISK_BAD_FAT32,
                           error, error_size, "FAT32 overhead exceeds volume");
        goto done;
    }
    layout.cluster_count = (uint32_t)((layout.total_sectors - overhead) /
                                      layout.sectors_per_cluster);
    layout.first_data_lba = partition_start + overhead;
    if (layout.cluster_count < FAT32_MIN_CLUSTERS ||
        layout.cluster_count > FAT32_MAX_CLUSTERS ||
        ((uint64_t)layout.fat_sectors * 512) / 4 <
            (uint64_t)layout.cluster_count + 2) {
        status = disk_fail(CVM_DISK_BAD_FAT32,
                           error, error_size, "invalid FAT32 cluster count");
        goto done;
    }

    size_t fat_bytes = (size_t)layout.fat_sectors * 512;
    uint8_t *fat = malloc(fat_bytes);
    uint8_t *fat_copy = malloc(fat_bytes);
    if (fat == NULL || fat_copy == NULL) {
        free(fat);
        free(fat_copy);
        status = disk_fail(CVM_DISK_OUT_OF_MEMORY,
                           error, error_size, "cannot allocate FAT tables");
        goto done;
    }
    uint64_t fat_lba = partition_start + FAT_RESERVED_SECTORS;
    if (!read_at(file, fat_lba * 512, fat, fat_bytes) ||
        !read_at(file, (fat_lba + layout.fat_sectors) * 512,
                 fat_copy, fat_bytes) ||
        memcmp(fat, fat_copy, fat_bytes) != 0 ||
        (read_u32_le(fat) & UINT32_C(0x0FFFFFFF)) !=
            UINT32_C(0x0FFFFFF8) ||
        (read_u32_le(fat + 8) & UINT32_C(0x0FFFFFFF)) <
            UINT32_C(0x0FFFFFF8)) {
        free(fat);
        free(fat_copy);
        status = disk_fail(CVM_DISK_BAD_FAT32,
                           error, error_size, "FAT tables are invalid");
        goto done;
    }
    free(fat_copy);

    size_t cluster_bytes = (size_t)layout.sectors_per_cluster * 512;
    uint8_t *directory = malloc(cluster_bytes);
    if (directory == NULL) {
        free(fat);
        status = disk_fail(CVM_DISK_OUT_OF_MEMORY,
                           error, error_size, "cannot allocate directory buffer");
        goto done;
    }
    uint32_t boot_cluster;
    uint32_t ignored_size;
    if (!read_cluster(file,
                      &layout,
                      FAT_ROOT_CLUSTER,
                      directory,
                      cluster_bytes) ||
        !find_directory_entry(directory,
                              cluster_bytes,
                              "BOOT       ",
                              0x10,
                              &boot_cluster,
                              &ignored_size)) {
        free(directory);
        free(fat);
        status = disk_fail(CVM_DISK_KERNEL_NOT_FOUND,
                           error, error_size, "BOOT directory is missing");
        goto done;
    }
    uint32_t bootloader_cluster;
    uint32_t bootloader_size;
    uint32_t kernel_cluster;
    uint32_t kernel_size;
    if (!read_cluster(file,
                      &layout,
                      boot_cluster,
                      directory,
                      cluster_bytes) ||
        !find_directory_entry(directory,
                              cluster_bytes,
                              "BOOT    CVM",
                              0x20,
                              &bootloader_cluster,
                              &bootloader_size) ||
        bootloader_size == 0 ||
        !find_directory_entry(directory,
                              cluster_bytes,
                              "KERNEL  CVM",
                              0x20,
                              &kernel_cluster,
                              &kernel_size) ||
        kernel_size == 0) {
        free(directory);
        free(fat);
        status = disk_fail(CVM_DISK_KERNEL_NOT_FOUND,
                           error, error_size,
                           "BOOT/BOOT.CVM or KERNEL.CVM is missing");
        goto done;
    }
    free(directory);
    status = validate_fat_image(file,
                                &layout,
                                fat,
                                bootloader_cluster,
                                bootloader_size,
                                CVM_DISK_BAD_BOOTLOADER,
                                "bootloader",
                                error,
                                error_size);
    if (status == CVM_DISK_OK) {
        status = validate_fat_image(file,
                                    &layout,
                                    fat,
                                    kernel_cluster,
                                    kernel_size,
                                    CVM_DISK_BAD_KERNEL,
                                    "kernel",
                                    error,
                                    error_size);
    }
    free(fat);
    if (status != CVM_DISK_OK) {
        goto done;
    }

    if (result != NULL) {
        memcpy(result->disk_guid, primary_header + 56, 16);
        memcpy(result->partition_guid, partition_guid, 16);
        result->disk_size = disk_size;
        result->total_sectors = total_sectors;
        result->partition_start_lba = partition_start;
        result->partition_sectors = partition_sectors;
        result->fat_sectors = layout.fat_sectors;
        result->sectors_per_cluster = layout.sectors_per_cluster;
        result->cluster_count = layout.cluster_count;
        result->bootloader_first_cluster = bootloader_cluster;
        result->bootloader_size = bootloader_size;
        result->kernel_first_cluster = kernel_cluster;
        result->kernel_size = kernel_size;
    }

done:
    if (fclose(file) != 0 && status == CVM_DISK_OK) {
        status = disk_fail(CVM_DISK_IO_ERROR,
                           error, error_size, "cannot close disk image");
    }
    return status;
}

const char *cvm_disk_status_name(CvmDiskStatus status)
{
    switch (status) {
    case CVM_DISK_OK:
        return "ok";
    case CVM_DISK_INVALID_ARGUMENT:
        return "invalid argument";
    case CVM_DISK_ALREADY_EXISTS:
        return "already exists";
    case CVM_DISK_BAD_SIZE:
        return "bad size";
    case CVM_DISK_BAD_BOOTLOADER:
        return "bad bootloader";
    case CVM_DISK_BAD_KERNEL:
        return "bad kernel";
    case CVM_DISK_BAD_GPT:
        return "bad GPT";
    case CVM_DISK_BAD_FAT32:
        return "bad FAT32";
    case CVM_DISK_KERNEL_NOT_FOUND:
        return "kernel not found";
    case CVM_DISK_OUT_OF_MEMORY:
        return "out of memory";
    case CVM_DISK_IO_ERROR:
        return "I/O error";
    default:
        return "unknown";
    }
}
