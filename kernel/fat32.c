#include "kernel_internal.h"

#define FAT_SECTOR_SIZE ((size_t)512)
#define FAT_ENTRY_END UINT32_C(0x0FFFFFFF)
#define FAT_ENTRY_MASK UINT32_C(0x0FFFFFFF)
#define FAT_ATTRIBUTE_DIRECTORY UINT8_C(0x10)
#define FAT_ATTRIBUTE_ARCHIVE UINT8_C(0x20)
#define FAT_ATTRIBUTE_LFN UINT8_C(0x0F)
#define FAT_FSINFO_LEAD UINT32_C(0x41615252)
#define FAT_FSINFO_STRUCTURE UINT32_C(0x61417272)
#define FAT_FSINFO_TRAIL UINT32_C(0xAA550000)
#define FAT_UNKNOWN_COUNT UINT32_C(0xFFFFFFFF)

typedef struct {
    int mounted;
    uint64_t partition_lba;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint64_t first_fat_lba;
    uint64_t first_data_lba;
    uint64_t fsinfo_lba;
    uint32_t next_free_cluster;
    uint32_t free_cluster_count;
    uint64_t cache_lba;
    uint8_t cache[FAT_SECTOR_SIZE];
    int fsinfo_valid;
    int cache_valid;
} KernelFat32;

typedef struct {
    uint8_t bytes[32];
    uint64_t sector_lba;
    size_t sector_offset;
    int found;
} FatDirectoryEntry;

static KernelFat32 kernel_fat;

static uint16_t fat_read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t fat_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void fat_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void fat_write_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}

static int fat_cluster_valid(uint32_t cluster)
{
    return cluster >= 2 && cluster < kernel_fat.total_clusters + 2;
}

static uint64_t fat_cluster_lba(uint32_t cluster)
{
    return kernel_fat.first_data_lba +
           (uint64_t)(cluster - 2) * kernel_fat.sectors_per_cluster;
}

static int fat_cache_load(uint64_t lba)
{
    if (kernel_fat.cache_valid && kernel_fat.cache_lba == lba) return 1;
    if (!kernel_block_read(lba, kernel_fat.cache, 1)) return 0;
    kernel_fat.cache_lba = lba;
    kernel_fat.cache_valid = 1;
    return 1;
}

static int fat_get(uint32_t cluster, uint32_t *value)
{
    if (!fat_cluster_valid(cluster) || value == NULL) return 0;
    uint64_t byte_offset = (uint64_t)cluster * 4;
    uint64_t lba = kernel_fat.first_fat_lba +
                   byte_offset / FAT_SECTOR_SIZE;
    if (!fat_cache_load(lba)) return 0;
    *value = fat_read_u32(kernel_fat.cache +
                         byte_offset % FAT_SECTOR_SIZE) &
             FAT_ENTRY_MASK;
    return 1;
}

static int fat_set(uint32_t cluster, uint32_t value)
{
    if (!fat_cluster_valid(cluster)) return 0;
    uint64_t byte_offset = (uint64_t)cluster * 4;
    for (uint32_t copy = 0; copy < kernel_fat.fat_count; ++copy) {
        uint64_t lba = kernel_fat.first_fat_lba +
                       (uint64_t)copy * kernel_fat.fat_sectors +
                       byte_offset / FAT_SECTOR_SIZE;
        uint8_t secondary[FAT_SECTOR_SIZE];
        uint8_t *sector;
        if (copy == 0) {
            if (!fat_cache_load(lba)) return 0;
            sector = kernel_fat.cache;
        } else {
            if (!kernel_block_read(lba, secondary, 1)) return 0;
            sector = secondary;
        }
        size_t offset = (size_t)(byte_offset % FAT_SECTOR_SIZE);
        uint32_t old = fat_read_u32(sector + offset);
        fat_write_u32(sector + offset,
                      (old & UINT32_C(0xF0000000)) |
                          (value & FAT_ENTRY_MASK));
        if (!kernel_block_write(lba, sector, 1)) return 0;
    }
    return 1;
}

static int fat_write_fsinfo(void)
{
    if (!kernel_fat.fsinfo_valid) return 1;
    uint8_t sector[FAT_SECTOR_SIZE];
    if (!kernel_block_read(kernel_fat.fsinfo_lba, sector, 1) ||
        fat_read_u32(sector) != FAT_FSINFO_LEAD ||
        fat_read_u32(sector + 484) != FAT_FSINFO_STRUCTURE ||
        fat_read_u32(sector + 508) != FAT_FSINFO_TRAIL) {
        return 0;
    }
    fat_write_u32(sector + 488, kernel_fat.free_cluster_count);
    fat_write_u32(sector + 492, kernel_fat.next_free_cluster);
    return kernel_block_write(kernel_fat.fsinfo_lba, sector, 1);
}

static uint32_t fat_entry_cluster(const uint8_t entry[32])
{
    return ((uint32_t)fat_read_u16(entry + 20) << 16) |
           fat_read_u16(entry + 26);
}

static int fat_short_name(const char *component,
                          size_t length,
                          uint8_t output[11])
{
    if (component == NULL || length == 0) return 0;
    for (size_t i = 0; i < 11; ++i) output[i] = (uint8_t)' ';
    size_t dot = length;
    for (size_t i = 0; i < length; ++i) {
        if (component[i] == '.') {
            if (dot != length) return 0;
            dot = i;
        }
    }
    size_t base_length = dot;
    size_t extension_length = dot < length ? length - dot - 1 : 0;
    if (base_length == 0 || base_length > 8 || extension_length > 3) return 0;
    for (size_t part = 0; part < length; ++part) {
        if (part == dot) continue;
        uint8_t character = (uint8_t)component[part];
        if (character >= (uint8_t)'a' && character <= (uint8_t)'z') {
            character = (uint8_t)(character - 'a' + 'A');
        }
        if (!((character >= (uint8_t)'A' && character <= (uint8_t)'Z') ||
              (character >= (uint8_t)'0' && character <= (uint8_t)'9') ||
              character == (uint8_t)'_' || character == (uint8_t)'-')) {
            return 0;
        }
        size_t destination = part < dot ? part : 8 + part - dot - 1;
        output[destination] = character;
    }
    return 1;
}

static int fat_name_equal(const uint8_t *entry, const uint8_t name[11])
{
    for (size_t i = 0; i < 11; ++i) {
        if (entry[i] != name[i]) return 0;
    }
    return 1;
}

static int fat_find_entry(uint32_t directory,
                          const uint8_t name[11],
                          FatDirectoryEntry *result,
                          FatDirectoryEntry *free_entry)
{
    if (!fat_cluster_valid(directory) || result == NULL) return 0;
    result->found = 0;
    if (free_entry != NULL) free_entry->found = 0;
    uint32_t cluster = directory;
    uint32_t traversed = 0;
    while (fat_cluster_valid(cluster) && traversed++ <= kernel_fat.total_clusters) {
        uint64_t first_lba = fat_cluster_lba(cluster);
        for (uint32_t sector_index = 0;
             sector_index < kernel_fat.sectors_per_cluster;
             ++sector_index) {
            uint8_t sector[FAT_SECTOR_SIZE];
            uint64_t lba = first_lba + sector_index;
            if (!kernel_block_read(lba, sector, 1)) return 0;
            for (size_t offset = 0; offset < FAT_SECTOR_SIZE; offset += 32) {
                const uint8_t *entry = sector + offset;
                if ((entry[0] == 0 || entry[0] == UINT8_C(0xE5)) &&
                    free_entry != NULL && !free_entry->found) {
                    for (size_t i = 0; i < 32; ++i) free_entry->bytes[i] = entry[i];
                    free_entry->sector_lba = lba;
                    free_entry->sector_offset = offset;
                    free_entry->found = 1;
                }
                if (entry[0] == 0) return 1;
                if (entry[0] == UINT8_C(0xE5) ||
                    entry[11] == FAT_ATTRIBUTE_LFN ||
                    (entry[11] & UINT8_C(0x08)) != 0) {
                    continue;
                }
                if (fat_name_equal(entry, name)) {
                    for (size_t i = 0; i < 32; ++i) result->bytes[i] = entry[i];
                    result->sector_lba = lba;
                    result->sector_offset = offset;
                    result->found = 1;
                    return 1;
                }
            }
        }
        uint32_t next;
        if (!fat_get(cluster, &next)) return 0;
        if (next >= UINT32_C(0x0FFFFFF8)) return 1;
        cluster = next;
    }
    return 0;
}

static int fat_resolve_parent(const char *path,
                              uint32_t *directory,
                              uint8_t final_name[11])
{
    if (path == NULL || directory == NULL || final_name == NULL ||
        path[0] != '/') return 0;
    uint32_t current = kernel_fat.root_cluster;
    const char *cursor = path + 1;
    for (;;) {
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') ++cursor;
        size_t length = (size_t)(cursor - start);
        uint8_t name[11];
        if (!fat_short_name(start, length, name)) return 0;
        if (*cursor == '\0') {
            for (size_t i = 0; i < 11; ++i) final_name[i] = name[i];
            *directory = current;
            return 1;
        }
        FatDirectoryEntry entry;
        if (!fat_find_entry(current, name, &entry, NULL) || !entry.found ||
            (entry.bytes[11] & FAT_ATTRIBUTE_DIRECTORY) == 0) {
            return 0;
        }
        current = fat_entry_cluster(entry.bytes);
        ++cursor;
        if (*cursor == '\0') return 0;
    }
}

static int fat_free_chain(uint32_t first)
{
    uint32_t cluster = first;
    uint32_t traversed = 0;
    while (fat_cluster_valid(cluster) && traversed++ <= kernel_fat.total_clusters) {
        uint32_t next;
        if (!fat_get(cluster, &next) || !fat_set(cluster, 0)) return 0;
        if (kernel_fat.free_cluster_count != FAT_UNKNOWN_COUNT) {
            ++kernel_fat.free_cluster_count;
        }
        if (!fat_cluster_valid(kernel_fat.next_free_cluster) ||
            cluster < kernel_fat.next_free_cluster) {
            kernel_fat.next_free_cluster = cluster;
        }
        if (next >= UINT32_C(0x0FFFFFF8)) return 1;
        cluster = next;
    }
    return first == 0;
}

static int fat_allocate_chain(size_t count, uint32_t *first)
{
    if (first == NULL) return 0;
    *first = 0;
    uint32_t previous = 0;
    size_t allocated = 0;
    uint32_t cluster = fat_cluster_valid(kernel_fat.next_free_cluster)
                           ? kernel_fat.next_free_cluster : 2;
    for (uint32_t scanned = 0;
         scanned < kernel_fat.total_clusters && allocated < count;
         ++scanned) {
        uint32_t value;
        if (!fat_get(cluster, &value)) break;
        if (value == 0) {
            if (!fat_set(cluster, FAT_ENTRY_END)) break;
            if (previous != 0 && !fat_set(previous, cluster)) {
                (void)fat_set(cluster, 0);
                break;
            }
            if (*first == 0) *first = cluster;
            previous = cluster;
            ++allocated;
            if (kernel_fat.free_cluster_count != FAT_UNKNOWN_COUNT &&
                kernel_fat.free_cluster_count != 0) {
                --kernel_fat.free_cluster_count;
            }
        }
        ++cluster;
        if (!fat_cluster_valid(cluster)) cluster = 2;
    }
    kernel_fat.next_free_cluster = cluster;
    if (allocated == count) return 1;
    if (*first != 0) (void)fat_free_chain(*first);
    *first = 0;
    return 0;
}

static int fat_write_chain(uint32_t first,
                           const uint8_t *data,
                           size_t size)
{
    uint32_t cluster = first;
    size_t consumed = 0;
    while (consumed < size && fat_cluster_valid(cluster)) {
        uint64_t lba = fat_cluster_lba(cluster);
        for (uint32_t sector_index = 0;
             sector_index < kernel_fat.sectors_per_cluster;
             ++sector_index) {
            uint8_t sector[FAT_SECTOR_SIZE];
            for (size_t i = 0; i < FAT_SECTOR_SIZE; ++i) sector[i] = 0;
            size_t remaining = size - consumed;
            size_t copy = remaining < FAT_SECTOR_SIZE ? remaining : FAT_SECTOR_SIZE;
            for (size_t i = 0; i < copy; ++i) sector[i] = data[consumed + i];
            if (!kernel_block_write(lba + sector_index, sector, 1)) return 0;
            consumed += copy;
            if (consumed == size) break;
        }
        if (consumed < size) {
            uint32_t next;
            if (!fat_get(cluster, &next)) return 0;
            cluster = next;
        }
    }
    return consumed == size;
}

int kernel_fat32_mount(void)
{
    uint8_t sector[FAT_SECTOR_SIZE];
    uint64_t partition = kernel_boot_info->boot_partition_lba;
    if (partition == 0 || !kernel_block_read(partition, sector, 1) ||
        fat_read_u16(sector + 11) != FAT_SECTOR_SIZE ||
        sector[13] == 0 || (sector[13] & (sector[13] - 1)) != 0 ||
        fat_read_u16(sector + 14) == 0 || sector[16] == 0 ||
        fat_read_u32(sector + 36) == 0 ||
        fat_read_u32(sector + 44) < 2 ||
        sector[510] != UINT8_C(0x55) || sector[511] != UINT8_C(0xAA)) {
        return 0;
    }
    uint32_t total_sectors = fat_read_u16(sector + 19);
    if (total_sectors == 0) total_sectors = fat_read_u32(sector + 32);
    uint32_t reserved = fat_read_u16(sector + 14);
    uint32_t fsinfo_sector = fat_read_u16(sector + 48);
    uint32_t fat_count = sector[16];
    uint32_t fat_sectors = fat_read_u32(sector + 36);
    uint64_t overhead = (uint64_t)reserved +
                        (uint64_t)fat_count * fat_sectors;
    if (total_sectors <= overhead) return 0;
    kernel_fat.mounted = 1;
    kernel_fat.partition_lba = partition;
    kernel_fat.sectors_per_cluster = sector[13];
    kernel_fat.reserved_sectors = reserved;
    kernel_fat.fat_count = fat_count;
    kernel_fat.fat_sectors = fat_sectors;
    kernel_fat.root_cluster = fat_read_u32(sector + 44);
    kernel_fat.total_clusters =
        (uint32_t)((total_sectors - overhead) / sector[13]);
    kernel_fat.first_fat_lba = partition + reserved;
    kernel_fat.first_data_lba = partition + overhead;
    kernel_fat.next_free_cluster = 2;
    kernel_fat.free_cluster_count = FAT_UNKNOWN_COUNT;
    kernel_fat.cache_valid = 0;
    kernel_fat.fsinfo_valid = 0;
    if (fsinfo_sector != 0 && fsinfo_sector < reserved) {
        uint8_t fsinfo[FAT_SECTOR_SIZE];
        kernel_fat.fsinfo_lba = partition + fsinfo_sector;
        if (kernel_block_read(kernel_fat.fsinfo_lba, fsinfo, 1) &&
            fat_read_u32(fsinfo) == FAT_FSINFO_LEAD &&
            fat_read_u32(fsinfo + 484) == FAT_FSINFO_STRUCTURE &&
            fat_read_u32(fsinfo + 508) == FAT_FSINFO_TRAIL) {
            uint32_t hint = fat_read_u32(fsinfo + 492);
            if (fat_cluster_valid(hint)) {
                kernel_fat.next_free_cluster = hint;
            }
            kernel_fat.free_cluster_count = fat_read_u32(fsinfo + 488);
            kernel_fat.fsinfo_valid = 1;
        }
    }
    return kernel_fat.total_clusters >= UINT32_C(65525);
}

#if 0
/* The range reader below is the single maintained data path. This older
   run-coalescing whole-file implementation remains as a source reference. */
int kernel_fat32_read_file(const char *path,
                           void *buffer,
                           size_t capacity,
                           size_t *file_size)
{
    if (!kernel_fat.mounted || buffer == NULL || file_size == NULL) return 0;
    uint32_t directory;
    uint8_t name[11];
    if (!fat_resolve_parent(path, &directory, name)) return 0;
    FatDirectoryEntry entry;
    if (!fat_find_entry(directory, name, &entry, NULL) || !entry.found ||
        (entry.bytes[11] & FAT_ATTRIBUTE_DIRECTORY) != 0) return 0;
    uint32_t size = fat_read_u32(entry.bytes + 28);
    *file_size = size;
    if (size > capacity) return 0;
    uint8_t *output = buffer;
    size_t consumed = 0;
    uint32_t cluster = fat_entry_cluster(entry.bytes);
    uint32_t traversed = 0;
    uint8_t transfer[KERNEL_PAGE_SIZE];
    while (consumed < size && fat_cluster_valid(cluster) &&
           traversed <= kernel_fat.total_clusters) {
        uint32_t sectors_per_cluster = kernel_fat.sectors_per_cluster;
        if (sectors_per_cluster > KERNEL_PAGE_SIZE / FAT_SECTOR_SIZE) {
            uint64_t lba = fat_cluster_lba(cluster);
            for (uint32_t sector_index = 0;
                 sector_index < sectors_per_cluster && consumed < size;
                 ++sector_index) {
                if (!kernel_block_read(lba + sector_index, transfer, 1)) {
                    return 0;
                }
                size_t copy = size - consumed;
                if (copy > FAT_SECTOR_SIZE) copy = FAT_SECTOR_SIZE;
                for (size_t i = 0; i < copy; ++i) {
                    output[consumed + i] = transfer[i];
                }
                consumed += copy;
            }
            ++traversed;
            if (consumed < size) {
                uint32_t next;
                if (!fat_get(cluster, &next) ||
                    next >= UINT32_C(0x0FFFFFF8)) return 0;
                cluster = next;
            }
            continue;
        }
        uint32_t max_run = (uint32_t)(KERNEL_PAGE_SIZE / FAT_SECTOR_SIZE) /
                           sectors_per_cluster;
        if (max_run == 0) max_run = 1;
        size_t cluster_bytes = (size_t)sectors_per_cluster * FAT_SECTOR_SIZE;
        size_t clusters_remaining =
            (size - consumed + cluster_bytes - 1) / cluster_bytes;
        uint32_t run = 1;
        uint32_t last = cluster;
        while (run < max_run && run < clusters_remaining) {
            uint32_t next;
            if (!fat_get(last, &next)) return 0;
            if (next != last + 1 || !fat_cluster_valid(next)) break;
            last = next;
            ++run;
        }
        size_t sector_count = (size_t)run * sectors_per_cluster;
        if (sector_count > KERNEL_PAGE_SIZE / FAT_SECTOR_SIZE) {
            sector_count = KERNEL_PAGE_SIZE / FAT_SECTOR_SIZE;
            run = 1;
        }
        if (!kernel_block_read(fat_cluster_lba(cluster), transfer,
                               sector_count)) return 0;
        size_t transfer_bytes = sector_count * FAT_SECTOR_SIZE;
        size_t copy = size - consumed;
        if (copy > transfer_bytes) copy = transfer_bytes;
        for (size_t i = 0; i < copy; ++i) {
            output[consumed + i] = transfer[i];
        }
        consumed += copy;
        traversed += run;
        if (consumed < size) {
            uint32_t next;
            if (!fat_get(last, &next) || next >= UINT32_C(0x0FFFFFF8)) {
                return 0;
            }
            cluster = next;
        }
    }
    return consumed == size;
}
#endif

int kernel_fat32_file_size(const char *path, size_t *file_size)
{
    if (!kernel_fat.mounted || file_size == NULL) return 0;
    uint32_t directory;
    uint8_t name[11];
    if (!fat_resolve_parent(path, &directory, name)) return 0;
    FatDirectoryEntry entry;
    if (!fat_find_entry(directory, name, &entry, NULL) || !entry.found ||
        (entry.bytes[11] & FAT_ATTRIBUTE_DIRECTORY) != 0) {
        return 0;
    }
    *file_size = (size_t)fat_read_u32(entry.bytes + 28);
    return 1;
}

int kernel_fat32_read_range(const char *path,
                            size_t offset,
                            void *buffer,
                            size_t capacity,
                            size_t *read_size)
{
    if (!kernel_fat.mounted || read_size == NULL ||
        (buffer == NULL && capacity != 0)) {
        return 0;
    }
    *read_size = 0;
    uint32_t directory;
    uint8_t name[11];
    if (!fat_resolve_parent(path, &directory, name)) return 0;
    FatDirectoryEntry entry;
    if (!fat_find_entry(directory, name, &entry, NULL) || !entry.found ||
        (entry.bytes[11] & FAT_ATTRIBUTE_DIRECTORY) != 0) {
        return 0;
    }
    size_t file_size = fat_read_u32(entry.bytes + 28);
    if (capacity == 0 || offset >= file_size) return 1;
    size_t amount = file_size - offset;
    if (amount > capacity) amount = capacity;
    size_t cluster_bytes = (size_t)kernel_fat.sectors_per_cluster *
                           FAT_SECTOR_SIZE;
    size_t skip_clusters = offset / cluster_bytes;
    size_t cluster_offset = offset % cluster_bytes;
    uint32_t cluster = fat_entry_cluster(entry.bytes);
    uint32_t traversed = 0;
    while (skip_clusters != 0) {
        uint32_t next;
        if (!fat_cluster_valid(cluster) || !fat_get(cluster, &next) ||
            next >= UINT32_C(0x0FFFFFF8) ||
            ++traversed > kernel_fat.total_clusters) {
            return 0;
        }
        cluster = next;
        --skip_clusters;
    }
    uint8_t sector[FAT_SECTOR_SIZE];
    uint8_t *output = buffer;
    size_t copied = 0;
    while (copied < amount && fat_cluster_valid(cluster)) {
        uint32_t sector_index = (uint32_t)(cluster_offset / FAT_SECTOR_SIZE);
        size_t sector_offset = cluster_offset % FAT_SECTOR_SIZE;
        while (sector_index < kernel_fat.sectors_per_cluster &&
               copied < amount) {
            if (!kernel_block_read(fat_cluster_lba(cluster) + sector_index,
                                   sector, 1)) {
                return 0;
            }
            size_t chunk = FAT_SECTOR_SIZE - sector_offset;
            if (chunk > amount - copied) chunk = amount - copied;
            for (size_t i = 0; i < chunk; ++i) {
                output[copied + i] = sector[sector_offset + i];
            }
            copied += chunk;
            ++sector_index;
            sector_offset = 0;
        }
        cluster_offset = 0;
        if (copied < amount) {
            uint32_t next;
            if (!fat_get(cluster, &next) || next >= UINT32_C(0x0FFFFFF8) ||
                ++traversed > kernel_fat.total_clusters) {
                return 0;
            }
            cluster = next;
        }
    }
    *read_size = copied;
    return copied == amount;
}

int kernel_fat32_read_file(const char *path,
                           void *buffer,
                           size_t capacity,
                           size_t *file_size)
{
    if (file_size == NULL || !kernel_fat32_file_size(path, file_size) ||
        *file_size > capacity) {
        return 0;
    }
    size_t read_size;
    return kernel_fat32_read_range(path, 0, buffer, *file_size, &read_size) &&
           read_size == *file_size;
}

int kernel_fat32_write_file(const char *path,
                            const void *buffer,
                            size_t size)
{
    if (!kernel_fat.mounted || kernel_block_read_only() ||
        (buffer == NULL && size != 0) || size > UINT32_MAX) return 0;
    uint32_t directory;
    uint8_t name[11];
    if (!fat_resolve_parent(path, &directory, name)) return 0;
    FatDirectoryEntry existing;
    FatDirectoryEntry free_entry;
    if (!fat_find_entry(directory, name, &existing, &free_entry)) return 0;
    FatDirectoryEntry *target = existing.found ? &existing : &free_entry;
    if (!target->found ||
        (existing.found &&
         (existing.bytes[11] & FAT_ATTRIBUTE_DIRECTORY) != 0)) return 0;

    size_t cluster_bytes = (size_t)kernel_fat.sectors_per_cluster *
                           FAT_SECTOR_SIZE;
    size_t cluster_count = size == 0 ? 0 :
                           (size + cluster_bytes - 1) / cluster_bytes;
    uint32_t new_first = 0;
    if (cluster_count != 0 &&
        (!fat_allocate_chain(cluster_count, &new_first) ||
         !fat_write_chain(new_first, buffer, size))) {
        if (new_first != 0) (void)fat_free_chain(new_first);
        return 0;
    }

    uint8_t sector[FAT_SECTOR_SIZE];
    if (!kernel_block_read(target->sector_lba, sector, 1)) {
        if (new_first != 0) (void)fat_free_chain(new_first);
        return 0;
    }
    uint8_t *entry = sector + target->sector_offset;
    for (size_t i = 0; i < 32; ++i) entry[i] = 0;
    for (size_t i = 0; i < 11; ++i) entry[i] = name[i];
    entry[11] = FAT_ATTRIBUTE_ARCHIVE;
    fat_write_u16(entry + 20, (uint16_t)(new_first >> 16));
    fat_write_u16(entry + 26, (uint16_t)new_first);
    fat_write_u32(entry + 28, (uint32_t)size);
    if (!kernel_block_write(target->sector_lba, sector, 1) ||
        !kernel_block_flush()) {
        if (new_first != 0) (void)fat_free_chain(new_first);
        return 0;
    }
    if (existing.found) {
        uint32_t old_first = fat_entry_cluster(existing.bytes);
        if (old_first != 0 && !fat_free_chain(old_first)) return 0;
        if (!fat_write_fsinfo() || !kernel_block_flush()) return 0;
    } else if (!fat_write_fsinfo() || !kernel_block_flush()) {
        return 0;
    }
    return 1;
}
