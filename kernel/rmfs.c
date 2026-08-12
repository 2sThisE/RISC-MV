#include "kernel_internal.h"

#include "rmfs_format.h"

#define RMFS_SECTOR_SIZE ((size_t)512)
#define RMFS_METADATA_CACHE_ENTRIES 64U
#define RMFS_GROUP_COMMIT_LIMIT 32U

typedef struct {
    uint64_t logical;
    uint64_t physical;
    uint32_t count;
} RmfsExtent;

typedef struct {
    uint16_t mode;
    uint32_t extent_count;
    uint64_t size;
    uint64_t parent;
    RmfsExtent extents[RMFS_INLINE_EXTENTS];
} RmfsInode;

typedef struct {
    uint64_t block;
    uint64_t age;
    int valid;
    int dirty;
    uint8_t data[RMFS_BLOCK_SIZE];
} RmfsMetadataCacheEntry;

typedef struct {
    int mounted;
    uint64_t partition_lba;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t inode_count;
    uint64_t free_inodes;
    uint64_t root_inode;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t block_bitmap_start;
    uint64_t block_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_start;
    uint64_t backup_block;
    uint64_t sequence;
    uint8_t *inode_bitmap;
    uint8_t *block_bitmap;
    uint8_t *inode_bitmap_dirty;
    uint8_t *block_bitmap_dirty;
    RmfsMetadataCacheEntry *metadata_cache;
    uint64_t metadata_cache_age;
    uint32_t pending_transactions;
    int group_active;
    uint8_t uuid[16];
    uint8_t scratch[RMFS_BLOCK_SIZE];
    uint8_t scratch2[RMFS_BLOCK_SIZE];
    KernelSpinLock lock;
} KernelRmfs;

typedef struct {
    uint64_t inode;
    uint64_t block;
    size_t offset;
    uint64_t logical_offset;
    int found;
    int free_found;
    uint64_t free_block;
    size_t free_offset;
} RmfsDirectoryResult;

static KernelRmfs kernel_rmfs;
static uint32_t rmfs_crc32_table[256];
static int rmfs_crc32_table_ready;

static uint16_t rmfs_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t rmfs_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t rmfs_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

static void rmfs_put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void rmfs_put_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}

static void rmfs_put_u64(uint8_t *bytes, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}

static void rmfs_crc32_init(void)
{
    if (rmfs_crc32_table_ready) return;
    for (uint32_t value = 0; value < 256; ++value) {
        uint32_t crc = value;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
        rmfs_crc32_table[value] = crc;
    }
    rmfs_crc32_table_ready = 1;
}

static uint32_t rmfs_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t i = 0; i < size; ++i) {
        crc = rmfs_crc32_table[(crc ^ data[i]) & UINT32_C(0xFF)] ^
              (crc >> 8);
    }
    return ~crc;
}

static int rmfs_checksum_valid(uint8_t *data, size_t size, size_t offset)
{
    uint32_t stored = rmfs_u32(data + offset);
    rmfs_put_u32(data + offset, 0);
    uint32_t actual = rmfs_crc32(data, size);
    rmfs_put_u32(data + offset, stored);
    return stored == actual;
}

static void rmfs_finish_checksum(uint8_t *data, size_t size, size_t offset)
{
    rmfs_put_u32(data + offset, 0);
    rmfs_put_u32(data + offset, rmfs_crc32(data, size));
}

static int rmfs_read_block(uint64_t block, uint8_t data[RMFS_BLOCK_SIZE])
{
    return block < kernel_rmfs.total_blocks &&
           kernel_block_read(kernel_rmfs.partition_lba +
                                 block * RMFS_SECTORS_PER_BLOCK,
                             data,
                             RMFS_SECTORS_PER_BLOCK);
}

static int rmfs_write_block(uint64_t block,
                            const uint8_t data[RMFS_BLOCK_SIZE])
{
    return block < kernel_rmfs.total_blocks &&
           kernel_block_write(kernel_rmfs.partition_lba +
                                  block * RMFS_SECTORS_PER_BLOCK,
                              data,
                              RMFS_SECTORS_PER_BLOCK);
}

static uint8_t *rmfs_metadata_block(uint64_t block, int dirty)
{
    RmfsMetadataCacheEntry *victim = NULL;
    if (kernel_rmfs.metadata_cache == NULL) return NULL;
    for (size_t i = 0; i < RMFS_METADATA_CACHE_ENTRIES; ++i) {
        RmfsMetadataCacheEntry *entry = &kernel_rmfs.metadata_cache[i];
        if (entry->valid && entry->block == block) {
            entry->age = ++kernel_rmfs.metadata_cache_age;
            if (dirty) entry->dirty = 1;
            return entry->data;
        }
        if (!entry->valid || victim == NULL || entry->age < victim->age) {
            victim = entry;
            if (!entry->valid) break;
        }
    }
    if (victim == NULL) return NULL;
    if (victim->valid && victim->dirty) {
        if (!kernel_rmfs.group_active ||
            !rmfs_write_block(victim->block, victim->data)) {
            return NULL;
        }
    }
    if (!rmfs_read_block(block, victim->data)) return NULL;
    victim->block = block;
    victim->age = ++kernel_rmfs.metadata_cache_age;
    victim->valid = 1;
    victim->dirty = dirty;
    return victim->data;
}

static int rmfs_flush_metadata_cache(void)
{
    if (kernel_rmfs.metadata_cache == NULL) return 0;
    for (size_t i = 0; i < RMFS_METADATA_CACHE_ENTRIES; ++i) {
        RmfsMetadataCacheEntry *entry = &kernel_rmfs.metadata_cache[i];
        if (entry->valid && entry->dirty) {
            if (!rmfs_write_block(entry->block, entry->data)) return 0;
            entry->dirty = 0;
        }
    }
    return 1;
}

static int rmfs_type_guid_equal(const uint8_t *entry)
{
    static const uint8_t type_guid[16] = {
        0xE4, 0xD7, 0x92, 0x3A, 0x2B, 0x1D, 0x68, 0x4C,
        0x9A, 0x6F, 0x52, 0x49, 0x53, 0x43, 0x4D, 0x56
    };
    for (size_t i = 0; i < 16; ++i) {
        if (entry[i] != type_guid[i]) return 0;
    }
    return 1;
}

static uint64_t rmfs_find_partition(void)
{
    uint8_t sector[RMFS_SECTOR_SIZE];
    for (uint64_t sector_index = 0; sector_index < 32; ++sector_index) {
        if (!kernel_block_read(UINT64_C(2) + sector_index, sector, 1)) return 0;
        for (size_t offset = 0; offset < RMFS_SECTOR_SIZE; offset += 128) {
            if (rmfs_type_guid_equal(sector + offset)) {
                return rmfs_u64(sector + offset + 32);
            }
        }
    }
    return 0;
}

static int rmfs_read_inode_raw(uint64_t number,
                               uint8_t bytes[RMFS_INODE_SIZE])
{
    if (number == 0 || number > kernel_rmfs.inode_count) return 0;
    uint64_t index = number - 1;
    uint64_t byte_offset = index * RMFS_INODE_SIZE;
    uint64_t block_number = kernel_rmfs.inode_table_start +
                            byte_offset / RMFS_BLOCK_SIZE;
    size_t block_offset = (size_t)(byte_offset % RMFS_BLOCK_SIZE);
    uint8_t *block = rmfs_metadata_block(block_number, 0);
    if (block == NULL) return 0;
    for (size_t i = 0; i < RMFS_INODE_SIZE; ++i) {
        bytes[i] = block[block_offset + i];
    }
    return rmfs_checksum_valid(bytes, RMFS_INODE_SIZE, RMFS_IN_CHECKSUM);
}

static int rmfs_decode_inode(const uint8_t bytes[RMFS_INODE_SIZE],
                             RmfsInode *inode)
{
    if (inode == NULL) return 0;
    inode->mode = rmfs_u16(bytes + RMFS_IN_MODE);
    inode->extent_count = rmfs_u32(bytes + RMFS_IN_EXTENT_COUNT);
    inode->size = rmfs_u64(bytes + RMFS_IN_SIZE);
    inode->parent = rmfs_u64(bytes + RMFS_IN_PARENT);
    if (inode->extent_count > RMFS_INLINE_EXTENTS) return 0;
    uint64_t next_logical = 0;
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        const uint8_t *extent = bytes + RMFS_IN_EXTENTS +
                                (size_t)i * RMFS_EXTENT_SIZE;
        inode->extents[i].logical = rmfs_u64(extent + RMFS_EX_LOGICAL_BLOCK);
        inode->extents[i].physical = rmfs_u64(extent + RMFS_EX_PHYSICAL_BLOCK);
        inode->extents[i].count = rmfs_u32(extent + RMFS_EX_BLOCK_COUNT);
        if (inode->extents[i].count == 0 ||
            inode->extents[i].logical != next_logical ||
            inode->extents[i].physical < kernel_rmfs.data_start ||
            inode->extents[i].physical >= kernel_rmfs.backup_block ||
            inode->extents[i].count >
                kernel_rmfs.backup_block - inode->extents[i].physical) {
            return 0;
        }
        next_logical += inode->extents[i].count;
    }
    uint64_t required_blocks = inode->size / RMFS_BLOCK_SIZE +
                               (inode->size % RMFS_BLOCK_SIZE != 0);
    return next_logical >= required_blocks;
}

static int rmfs_read_inode(uint64_t number, RmfsInode *inode)
{
    uint8_t bytes[RMFS_INODE_SIZE];
    return rmfs_read_inode_raw(number, bytes) &&
           rmfs_decode_inode(bytes, inode);
}

static void rmfs_copy_inode(RmfsInode *destination,
                            const RmfsInode *source)
{
    destination->mode = source->mode;
    destination->extent_count = source->extent_count;
    destination->size = source->size;
    destination->parent = source->parent;
    for (uint32_t i = 0; i < source->extent_count; ++i) {
        destination->extents[i].logical = source->extents[i].logical;
        destination->extents[i].physical = source->extents[i].physical;
        destination->extents[i].count = source->extents[i].count;
    }
}

static int rmfs_append_extent(RmfsInode *inode,
                              uint64_t logical,
                              uint64_t physical,
                              uint32_t count)
{
    if (count == 0) return 1;
    if (inode->extent_count != 0) {
        RmfsExtent *last = &inode->extents[inode->extent_count - 1];
        if (last->logical + last->count != logical) return 0;
        if (last->physical + last->count == physical &&
            count <= UINT32_MAX - last->count) {
            last->count += count;
            return 1;
        }
    }
    if (inode->extent_count == RMFS_INLINE_EXTENTS) return 0;
    RmfsExtent *extent = &inode->extents[inode->extent_count++];
    extent->logical = logical;
    extent->physical = physical;
    extent->count = count;
    return 1;
}

static int rmfs_append_inode_range(RmfsInode *destination,
                                   const RmfsInode *source,
                                   uint64_t first,
                                   uint64_t end)
{
    if (first >= end) return 1;
    uint64_t covered = first;
    for (uint32_t i = 0; i < source->extent_count; ++i) {
        const RmfsExtent *extent = &source->extents[i];
        uint64_t extent_end = extent->logical + extent->count;
        uint64_t overlap_first = first > extent->logical
                                     ? first : extent->logical;
        uint64_t overlap_end = end < extent_end ? end : extent_end;
        if (overlap_first >= overlap_end) continue;
        if (overlap_first != covered) return 0;
        uint64_t count = overlap_end - overlap_first;
        if (count > UINT32_MAX ||
            !rmfs_append_extent(destination, overlap_first,
                                extent->physical +
                                    overlap_first - extent->logical,
                                (uint32_t)count)) {
            return 0;
        }
        covered = overlap_end;
    }
    return covered == end;
}

static int rmfs_append_allocated(RmfsInode *destination,
                                 const RmfsInode *allocation,
                                 uint64_t logical_base)
{
    for (uint32_t i = 0; i < allocation->extent_count; ++i) {
        const RmfsExtent *extent = &allocation->extents[i];
        if (extent->logical > UINT64_MAX - logical_base ||
            !rmfs_append_extent(destination,
                                logical_base + extent->logical,
                                extent->physical, extent->count)) {
            return 0;
        }
    }
    return 1;
}

static uint64_t rmfs_inode_block_count(const RmfsInode *inode)
{
    uint64_t count = 0;
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        count += inode->extents[i].count;
    }
    return count;
}

static uint64_t rmfs_inode_physical_block(const RmfsInode *inode,
                                          uint64_t logical)
{
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        const RmfsExtent *extent = &inode->extents[i];
        if (logical >= extent->logical &&
            logical - extent->logical < extent->count) {
            return extent->physical + logical - extent->logical;
        }
    }
    return 0;
}

static int rmfs_name_equal(const uint8_t *entry,
                           const char *name,
                           size_t length)
{
    if (rmfs_u16(entry + RMFS_DE_NAME_LENGTH) != length) return 0;
    for (size_t i = 0; i < length; ++i) {
        if (entry[RMFS_DE_NAME + i] != (uint8_t)name[i]) return 0;
    }
    return 1;
}

static int rmfs_find_directory_entry(const RmfsInode *directory,
                                     const char *name,
                                     size_t length,
                                     RmfsDirectoryResult *result)
{
    if (directory == NULL || result == NULL || length == 0 ||
        length > RMFS_NAME_MAX ||
        (directory->mode & RMFS_MODE_TYPE_MASK) != RMFS_MODE_DIRECTORY) {
        return 0;
    }
    result->found = 0;
    result->free_found = 0;
    uint64_t scanned = 0;
    for (uint32_t extent_index = 0;
         extent_index < directory->extent_count;
         ++extent_index) {
        const RmfsExtent *extent = &directory->extents[extent_index];
        for (uint32_t block_index = 0;
             block_index < extent->count && scanned < directory->size;
            ++block_index) {
            uint64_t physical = extent->physical + block_index;
            uint8_t *block = rmfs_metadata_block(physical, 0);
            if (block == NULL) return 0;
            for (size_t offset = 0;
                 offset + RMFS_DIRENT_SIZE <= RMFS_BLOCK_SIZE &&
                 scanned < directory->size;
                 offset += RMFS_DIRENT_SIZE, scanned += RMFS_DIRENT_SIZE) {
                uint8_t *entry = block + offset;
                uint64_t number = rmfs_u64(entry + RMFS_DE_INODE);
                if (number == 0) {
                    if (!result->free_found) {
                        result->free_found = 1;
                        result->free_block = physical;
                        result->free_offset = offset;
                        result->logical_offset = scanned;
                    }
                    continue;
                }
                if (!rmfs_checksum_valid(entry, RMFS_DIRENT_SIZE,
                                         RMFS_DE_CHECKSUM)) return 0;
                if (rmfs_name_equal(entry, name, length)) {
                    result->inode = number;
                    result->block = physical;
                    result->offset = offset;
                    result->logical_offset = scanned;
                    result->found = 1;
                    return 1;
                }
            }
        }
    }
    /* The initialized directory block contains unused slots beyond i_size. */
    if (!result->free_found && directory->extent_count != 0) {
        uint64_t capacity = rmfs_inode_block_count(directory) * RMFS_BLOCK_SIZE;
        if (directory->size < capacity) {
            uint64_t logical = directory->size / RMFS_BLOCK_SIZE;
            result->free_found = 1;
            result->free_block = rmfs_inode_physical_block(directory, logical);
            result->free_offset = (size_t)(directory->size % RMFS_BLOCK_SIZE);
            result->logical_offset = directory->size;
            if (result->free_block == 0) return 0;
        }
    }
    return 1;
}

static int rmfs_resolve(const char *path,
                        uint64_t *number,
                        RmfsInode *inode)
{
    if (!kernel_rmfs.mounted || path == NULL || path[0] != '/' ||
        number == NULL || inode == NULL) return 0;
    uint64_t current_number = kernel_rmfs.root_inode;
    RmfsInode current;
    if (!rmfs_read_inode(current_number, &current)) return 0;
    const char *cursor = path + 1;
    if (*cursor == '\0') {
        *number = current_number;
        rmfs_copy_inode(inode, &current);
        return 1;
    }
    for (;;) {
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') ++cursor;
        size_t length = (size_t)(cursor - start);
        if (length == 0) return 0;
        RmfsDirectoryResult entry;
        if (!rmfs_find_directory_entry(&current, start, length, &entry) ||
            !entry.found || !rmfs_read_inode(entry.inode, &current)) return 0;
        current_number = entry.inode;
        if (*cursor == '\0') {
            *number = current_number;
            rmfs_copy_inode(inode, &current);
            return 1;
        }
        if ((current.mode & RMFS_MODE_TYPE_MASK) != RMFS_MODE_DIRECTORY) return 0;
        ++cursor;
    }
}

static int rmfs_resolve_parent(const char *path,
                               uint64_t *parent_number,
                               RmfsInode *parent,
                               const char **name,
                               size_t *name_length)
{
    if (path == NULL || path[0] != '/' || path[1] == '\0' ||
        parent_number == NULL || parent == NULL || name == NULL ||
        name_length == NULL) return 0;
    const char *last = path + 1;
    const char *cursor = path + 1;
    while (*cursor != '\0') {
        if (*cursor == '/' && cursor[1] != '\0') last = cursor + 1;
        ++cursor;
    }
    *name_length = (size_t)(cursor - last);
    *name = last;
    if (*name_length == 0 || *name_length > RMFS_NAME_MAX) return 0;
    if (last == path + 1) {
        *parent_number = kernel_rmfs.root_inode;
        return rmfs_read_inode(*parent_number, parent);
    }
    size_t parent_length = (size_t)(last - path - 1);
    if (parent_length >= 128) return 0;
    char parent_path[128];
    for (size_t i = 0; i < parent_length; ++i) parent_path[i] = path[i];
    parent_path[parent_length] = '\0';
    return rmfs_resolve(parent_path, parent_number, parent) &&
           (parent->mode & RMFS_MODE_TYPE_MASK) == RMFS_MODE_DIRECTORY;
}

static int rmfs_bitmap_get(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit / 8] &
            (uint8_t)(UINT8_C(1) << (bit % 8))) != 0;
}

static void rmfs_bitmap_set(uint8_t *bitmap, uint64_t bit, int used)
{
    uint8_t mask = (uint8_t)(UINT8_C(1) << (bit % 8));
    if (used) bitmap[bit / 8] |= mask;
    else bitmap[bit / 8] &= (uint8_t)~mask;
}

static uint8_t *rmfs_load_bitmap(uint64_t start, uint64_t blocks)
{
    if (blocks == 0 || blocks > KERNEL_SIZE_MAX / RMFS_BLOCK_SIZE) return NULL;
    uint8_t *bitmap = kernel_malloc((size_t)blocks * RMFS_BLOCK_SIZE);
    if (bitmap == NULL) return NULL;
    for (uint64_t i = 0; i < blocks; ++i) {
        if (!rmfs_read_block(start + i, bitmap + (size_t)i * RMFS_BLOCK_SIZE)) {
            kernel_free(bitmap);
            return NULL;
        }
    }
    return bitmap;
}

static int rmfs_mark_bitmap_bit(uint8_t *dirty, uint64_t blocks,
                                uint64_t bit)
{
    uint64_t bits_per_block = (uint64_t)RMFS_BLOCK_SIZE * 8;
    uint64_t index = bit / bits_per_block;
    if (dirty == NULL || index >= blocks) return 0;
    dirty[index] = 1;
    return 1;
}

static int rmfs_mark_extent_bits(uint8_t *dirty, uint64_t blocks,
                                 const RmfsInode *inode)
{
    uint64_t bits_per_block = (uint64_t)RMFS_BLOCK_SIZE * 8;
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        uint64_t first = inode->extents[i].physical / bits_per_block;
        uint64_t last = (inode->extents[i].physical +
                         inode->extents[i].count - 1) / bits_per_block;
        for (uint64_t index = first; index <= last; ++index) {
            if (dirty == NULL || index >= blocks) return 0;
            dirty[index] = 1;
        }
    }
    return 1;
}

static int rmfs_allocate_extents(uint8_t *bitmap,
                                 uint64_t count,
                                 RmfsInode *inode)
{
    inode->extent_count = 0;
    if (count == 0) return 1;
    if (count > kernel_rmfs.free_blocks) return 0;
    uint64_t logical = 0;
    for (uint64_t block = kernel_rmfs.data_start;
         block < kernel_rmfs.backup_block && count != 0;) {
        if (rmfs_bitmap_get(bitmap, block)) {
            ++block;
            continue;
        }
        if (inode->extent_count == RMFS_INLINE_EXTENTS) break;
        uint64_t start = block;
        uint64_t run = 0;
        while (block < kernel_rmfs.backup_block && count != 0 &&
               !rmfs_bitmap_get(bitmap, block) && run < UINT32_MAX) {
            rmfs_bitmap_set(bitmap, block, 1);
            ++block;
            ++run;
            --count;
        }
        RmfsExtent *extent = &inode->extents[inode->extent_count++];
        extent->logical = logical;
        extent->physical = start;
        extent->count = (uint32_t)run;
        logical += run;
    }
    if (count == 0) return 1;
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        for (uint32_t j = 0; j < inode->extents[i].count; ++j) {
            rmfs_bitmap_set(bitmap, inode->extents[i].physical + j, 0);
        }
    }
    inode->extent_count = 0;
    return 0;
}

static uint64_t rmfs_allocate_inode(uint8_t *bitmap)
{
    for (uint64_t number = 1; number <= kernel_rmfs.inode_count; ++number) {
        if (!rmfs_bitmap_get(bitmap, number - 1)) {
            rmfs_bitmap_set(bitmap, number - 1, 1);
            return number;
        }
    }
    return 0;
}

static int rmfs_write_inode(uint64_t number, const RmfsInode *record)
{
    if (number == 0 || number > kernel_rmfs.inode_count || record == NULL ||
        record->extent_count > RMFS_INLINE_EXTENTS) return 0;
    uint64_t byte_offset = (number - 1) * RMFS_INODE_SIZE;
    uint64_t table_block = kernel_rmfs.inode_table_start +
                           byte_offset / RMFS_BLOCK_SIZE;
    size_t offset = (size_t)(byte_offset % RMFS_BLOCK_SIZE);
    uint8_t *block = rmfs_metadata_block(table_block, 1);
    if (block == NULL) return 0;
    uint8_t *inode = block + offset;
    for (size_t i = 0; i < RMFS_INODE_SIZE; ++i) inode[i] = 0;
    rmfs_put_u16(inode + RMFS_IN_MODE, record->mode);
    rmfs_put_u32(inode + RMFS_IN_LINK_COUNT, 1);
    rmfs_put_u32(inode + RMFS_IN_EXTENT_COUNT, record->extent_count);
    rmfs_put_u64(inode + RMFS_IN_SIZE, record->size);
    rmfs_put_u64(inode + RMFS_IN_GENERATION, kernel_rmfs.sequence + 1);
    rmfs_put_u64(inode + RMFS_IN_PARENT, record->parent);
    for (uint32_t i = 0; i < record->extent_count; ++i) {
        uint8_t *extent = inode + RMFS_IN_EXTENTS +
                          (size_t)i * RMFS_EXTENT_SIZE;
        rmfs_put_u64(extent + RMFS_EX_LOGICAL_BLOCK,
                     record->extents[i].logical);
        rmfs_put_u64(extent + RMFS_EX_PHYSICAL_BLOCK,
                     record->extents[i].physical);
        rmfs_put_u32(extent + RMFS_EX_BLOCK_COUNT,
                     record->extents[i].count);
    }
    rmfs_finish_checksum(inode, RMFS_INODE_SIZE, RMFS_IN_CHECKSUM);
    return rmfs_write_block(table_block, block);
}

static int rmfs_write_dirent(uint64_t block_number, size_t offset,
                             uint64_t inode_number, uint16_t mode,
                             const char *name, size_t length)
{
    if (offset + RMFS_DIRENT_SIZE > RMFS_BLOCK_SIZE) return 0;
    uint8_t *block = rmfs_metadata_block(block_number, 1);
    if (block == NULL) return 0;
    uint8_t *entry = block + offset;
    for (size_t i = 0; i < RMFS_DIRENT_SIZE; ++i) entry[i] = 0;
    rmfs_put_u64(entry + RMFS_DE_INODE, inode_number);
    rmfs_put_u16(entry + RMFS_DE_MODE, mode);
    rmfs_put_u16(entry + RMFS_DE_NAME_LENGTH, (uint16_t)length);
    for (size_t i = 0; i < length; ++i) entry[RMFS_DE_NAME + i] = (uint8_t)name[i];
    rmfs_finish_checksum(entry, RMFS_DIRENT_SIZE, RMFS_DE_CHECKSUM);
    return rmfs_write_block(block_number, block);
}

static uint64_t rmfs_release_extents(uint8_t *bitmap,
                                     const RmfsInode *inode)
{
    uint64_t released = 0;
    for (uint32_t i = 0; i < inode->extent_count; ++i) {
        for (uint32_t j = 0; j < inode->extents[i].count; ++j) {
            rmfs_bitmap_set(bitmap, inode->extents[i].physical + j, 0);
            ++released;
        }
    }
    return released;
}

static int rmfs_expand_directory(uint8_t *bitmap,
                                 RmfsInode *directory,
                                 RmfsDirectoryResult *entry)
{
    RmfsInode allocation;
    allocation.extent_count = 0;
    if (!rmfs_allocate_extents(bitmap, 1, &allocation) ||
        allocation.extent_count != 1) return 0;
    uint64_t new_block = allocation.extents[0].physical;
    uint64_t logical = rmfs_inode_block_count(directory);
    uint8_t *block = kernel_rmfs.scratch;
    for (size_t i = 0; i < RMFS_BLOCK_SIZE; ++i) block[i] = 0;
    if (!rmfs_write_block(new_block, block)) {
        rmfs_bitmap_set(bitmap, new_block, 0);
        return 0;
    }
    if (directory->extent_count != 0) {
        RmfsExtent *last = &directory->extents[directory->extent_count - 1];
        if (last->physical + last->count == new_block &&
            last->logical + last->count == logical &&
            last->count != UINT32_MAX) {
            ++last->count;
        } else if (directory->extent_count < RMFS_INLINE_EXTENTS) {
            RmfsExtent *next = &directory->extents[directory->extent_count++];
            next->logical = logical;
            next->physical = new_block;
            next->count = 1;
        } else {
            rmfs_bitmap_set(bitmap, new_block, 0);
            return 0;
        }
    } else {
        directory->extent_count = 1;
        directory->extents[0].logical = 0;
        directory->extents[0].physical = new_block;
        directory->extents[0].count = 1;
    }
    entry->free_found = 1;
    entry->free_block = new_block;
    entry->free_offset = 0;
    entry->logical_offset = directory->size;
    return 1;
}

static int rmfs_write_super(uint32_t state)
{
    uint8_t *block = kernel_rmfs.scratch;
    for (size_t i = 0; i < RMFS_BLOCK_SIZE; ++i) block[i] = 0;
    static const uint8_t magic[RMFS_MAGIC_SIZE] = RMFS_MAGIC;
    for (size_t i = 0; i < RMFS_MAGIC_SIZE; ++i) block[i] = magic[i];
    rmfs_put_u16(block + RMFS_SB_VERSION_MAJOR, RMFS_VERSION_MAJOR);
    rmfs_put_u16(block + RMFS_SB_VERSION_MINOR, RMFS_VERSION_MINOR);
    rmfs_put_u32(block + RMFS_SB_BLOCK_SIZE, RMFS_BLOCK_SIZE);
    rmfs_put_u32(block + RMFS_SB_INODE_SIZE, RMFS_INODE_SIZE);
    rmfs_put_u32(block + RMFS_SB_STATE, state);
    rmfs_put_u64(block + RMFS_SB_TOTAL_BLOCKS, kernel_rmfs.total_blocks);
    rmfs_put_u64(block + RMFS_SB_FREE_BLOCKS, kernel_rmfs.free_blocks);
    rmfs_put_u64(block + RMFS_SB_INODE_COUNT, kernel_rmfs.inode_count);
    rmfs_put_u64(block + RMFS_SB_FREE_INODES, kernel_rmfs.free_inodes);
    rmfs_put_u64(block + RMFS_SB_ROOT_INODE, kernel_rmfs.root_inode);
    rmfs_put_u64(block + RMFS_SB_INODE_BITMAP_START,
                 kernel_rmfs.inode_bitmap_start);
    rmfs_put_u64(block + RMFS_SB_INODE_BITMAP_BLOCKS,
                 kernel_rmfs.inode_bitmap_blocks);
    rmfs_put_u64(block + RMFS_SB_BLOCK_BITMAP_START,
                 kernel_rmfs.block_bitmap_start);
    rmfs_put_u64(block + RMFS_SB_BLOCK_BITMAP_BLOCKS,
                 kernel_rmfs.block_bitmap_blocks);
    rmfs_put_u64(block + RMFS_SB_INODE_TABLE_START,
                 kernel_rmfs.inode_table_start);
    rmfs_put_u64(block + RMFS_SB_INODE_TABLE_BLOCKS,
                 kernel_rmfs.inode_table_blocks);
    rmfs_put_u64(block + RMFS_SB_DATA_START, kernel_rmfs.data_start);
    rmfs_put_u64(block + RMFS_SB_BACKUP_BLOCK, kernel_rmfs.backup_block);
    rmfs_put_u64(block + RMFS_SB_SEQUENCE, kernel_rmfs.sequence);
    for (size_t i = 0; i < 16; ++i) block[RMFS_SB_UUID + i] = kernel_rmfs.uuid[i];
    rmfs_finish_checksum(block, RMFS_BLOCK_SIZE, RMFS_SB_CHECKSUM);
    return rmfs_write_block(0, block) &&
           rmfs_write_block(kernel_rmfs.backup_block, block);
}

static int rmfs_flush_bitmap(uint64_t start, uint64_t blocks,
                             const uint8_t *bitmap, uint8_t *dirty)
{
    if (bitmap == NULL || dirty == NULL) return 0;
    for (uint64_t index = 0; index < blocks; ++index) {
        if (dirty[index] == 0) continue;
        if (!rmfs_write_block(start + index,
                              bitmap + (size_t)index * RMFS_BLOCK_SIZE)) {
            return 0;
        }
        dirty[index] = 0;
    }
    return 1;
}

static int rmfs_begin_group(void)
{
    if (kernel_rmfs.group_active) return 1;
    if (!rmfs_write_super(RMFS_STATE_DIRTY) || !kernel_block_flush()) {
        kernel_rmfs.mounted = 0;
        return 0;
    }
    kernel_rmfs.group_active = 1;
    kernel_rmfs.pending_transactions = 0;
    return 1;
}

static int rmfs_sync_locked(void)
{
    if (!kernel_rmfs.mounted) return 0;
    if (!kernel_rmfs.group_active) return 1;
    int okay = rmfs_flush_metadata_cache() &&
               rmfs_flush_bitmap(kernel_rmfs.inode_bitmap_start,
                                 kernel_rmfs.inode_bitmap_blocks,
                                 kernel_rmfs.inode_bitmap,
                                 kernel_rmfs.inode_bitmap_dirty) &&
               rmfs_flush_bitmap(kernel_rmfs.block_bitmap_start,
                                 kernel_rmfs.block_bitmap_blocks,
                                 kernel_rmfs.block_bitmap,
                                 kernel_rmfs.block_bitmap_dirty) &&
               rmfs_write_super(RMFS_STATE_CLEAN) && kernel_block_flush();
    if (!okay) {
        kernel_rmfs.mounted = 0;
        return 0;
    }
    kernel_rmfs.group_active = 0;
    kernel_rmfs.pending_transactions = 0;
    return 1;
}

int kernel_rmfs_mount(void)
{
    rmfs_crc32_init();
    kernel_spin_init(&kernel_rmfs.lock);
    kernel_rmfs.mounted = 0;
    kernel_free(kernel_rmfs.inode_bitmap);
    kernel_free(kernel_rmfs.block_bitmap);
    kernel_free(kernel_rmfs.inode_bitmap_dirty);
    kernel_free(kernel_rmfs.block_bitmap_dirty);
    kernel_free(kernel_rmfs.metadata_cache);
    kernel_rmfs.inode_bitmap = NULL;
    kernel_rmfs.block_bitmap = NULL;
    kernel_rmfs.inode_bitmap_dirty = NULL;
    kernel_rmfs.block_bitmap_dirty = NULL;
    kernel_rmfs.metadata_cache = NULL;
    kernel_rmfs.metadata_cache_age = 0;
    kernel_rmfs.pending_transactions = 0;
    kernel_rmfs.group_active = 0;
    kernel_rmfs.partition_lba = rmfs_find_partition();
    if (kernel_rmfs.partition_lba == 0) return 0;
    /* Give the block helper geometry before using rmfs_read_block. */
    kernel_rmfs.total_blocks =
        (kernel_block_capacity() - kernel_rmfs.partition_lba) /
        RMFS_SECTORS_PER_BLOCK;
    uint8_t *block = kernel_rmfs.scratch;
    if (!rmfs_read_block(0, block)) return 0;
    static const uint8_t magic[RMFS_MAGIC_SIZE] = RMFS_MAGIC;
    for (size_t i = 0; i < RMFS_MAGIC_SIZE; ++i) {
        if (block[i] != magic[i]) return 0;
    }
    if (rmfs_u16(block + RMFS_SB_VERSION_MAJOR) != RMFS_VERSION_MAJOR ||
        rmfs_u32(block + RMFS_SB_BLOCK_SIZE) != RMFS_BLOCK_SIZE ||
        rmfs_u32(block + RMFS_SB_INODE_SIZE) != RMFS_INODE_SIZE ||
        rmfs_u32(block + RMFS_SB_STATE) != RMFS_STATE_CLEAN ||
        rmfs_u64(block + RMFS_SB_FEATURE_INCOMPAT) != 0 ||
        !rmfs_checksum_valid(block, RMFS_BLOCK_SIZE, RMFS_SB_CHECKSUM)) {
        return 0;
    }
    uint64_t volume_blocks = rmfs_u64(block + RMFS_SB_TOTAL_BLOCKS);
    if (volume_blocks == 0 || volume_blocks > kernel_rmfs.total_blocks) return 0;
    kernel_rmfs.total_blocks = volume_blocks;
    kernel_rmfs.free_blocks = rmfs_u64(block + RMFS_SB_FREE_BLOCKS);
    kernel_rmfs.inode_count = rmfs_u64(block + RMFS_SB_INODE_COUNT);
    kernel_rmfs.free_inodes = rmfs_u64(block + RMFS_SB_FREE_INODES);
    kernel_rmfs.root_inode = rmfs_u64(block + RMFS_SB_ROOT_INODE);
    kernel_rmfs.inode_bitmap_start = rmfs_u64(block + RMFS_SB_INODE_BITMAP_START);
    kernel_rmfs.inode_bitmap_blocks = rmfs_u64(block + RMFS_SB_INODE_BITMAP_BLOCKS);
    kernel_rmfs.block_bitmap_start = rmfs_u64(block + RMFS_SB_BLOCK_BITMAP_START);
    kernel_rmfs.block_bitmap_blocks = rmfs_u64(block + RMFS_SB_BLOCK_BITMAP_BLOCKS);
    kernel_rmfs.inode_table_start = rmfs_u64(block + RMFS_SB_INODE_TABLE_START);
    kernel_rmfs.inode_table_blocks = rmfs_u64(block + RMFS_SB_INODE_TABLE_BLOCKS);
    kernel_rmfs.data_start = rmfs_u64(block + RMFS_SB_DATA_START);
    kernel_rmfs.backup_block = rmfs_u64(block + RMFS_SB_BACKUP_BLOCK);
    kernel_rmfs.sequence = rmfs_u64(block + RMFS_SB_SEQUENCE);
    for (size_t i = 0; i < 16; ++i) kernel_rmfs.uuid[i] = block[RMFS_SB_UUID + i];
    uint64_t bitmap_bits = (uint64_t)RMFS_BLOCK_SIZE * 8;
    uint64_t expected_inode_bitmap =
        (kernel_rmfs.inode_count + bitmap_bits - 1) / bitmap_bits;
    uint64_t expected_block_bitmap =
        (kernel_rmfs.total_blocks + bitmap_bits - 1) / bitmap_bits;
    uint64_t expected_inode_table =
        (kernel_rmfs.inode_count + 15) / 16;
    if (kernel_rmfs.root_inode == 0 ||
        kernel_rmfs.root_inode > kernel_rmfs.inode_count ||
        kernel_rmfs.inode_count < RMFS_INIT_INODE ||
        kernel_rmfs.inode_count > RMFS_MAX_INODE_COUNT ||
        kernel_rmfs.inode_bitmap_start != 1 ||
        kernel_rmfs.inode_bitmap_blocks != expected_inode_bitmap ||
        kernel_rmfs.block_bitmap_start !=
            kernel_rmfs.inode_bitmap_start + expected_inode_bitmap ||
        kernel_rmfs.block_bitmap_blocks != expected_block_bitmap ||
        kernel_rmfs.inode_table_start !=
            kernel_rmfs.block_bitmap_start + expected_block_bitmap ||
        kernel_rmfs.inode_table_blocks != expected_inode_table ||
        kernel_rmfs.data_start !=
            kernel_rmfs.inode_table_start + expected_inode_table ||
        kernel_rmfs.backup_block != kernel_rmfs.total_blocks - 1 ||
        kernel_rmfs.data_start >= kernel_rmfs.backup_block ||
        kernel_rmfs.free_blocks > kernel_rmfs.total_blocks ||
        kernel_rmfs.free_inodes > kernel_rmfs.inode_count) return 0;
    uint8_t *backup = kernel_rmfs.scratch2;
    if (!rmfs_read_block(kernel_rmfs.backup_block, backup)) return 0;
    for (size_t i = 0; i < RMFS_BLOCK_SIZE; ++i) {
        if (block[i] != backup[i]) return 0;
    }
    kernel_rmfs.inode_bitmap = rmfs_load_bitmap(
        kernel_rmfs.inode_bitmap_start, kernel_rmfs.inode_bitmap_blocks);
    kernel_rmfs.block_bitmap = rmfs_load_bitmap(
        kernel_rmfs.block_bitmap_start, kernel_rmfs.block_bitmap_blocks);
    if (kernel_rmfs.inode_bitmap_blocks > KERNEL_SIZE_MAX ||
        kernel_rmfs.block_bitmap_blocks > KERNEL_SIZE_MAX) {
        kernel_free(kernel_rmfs.inode_bitmap);
        kernel_free(kernel_rmfs.block_bitmap);
        kernel_rmfs.inode_bitmap = NULL;
        kernel_rmfs.block_bitmap = NULL;
        return 0;
    }
    kernel_rmfs.inode_bitmap_dirty = kernel_calloc(
        (size_t)kernel_rmfs.inode_bitmap_blocks, 1);
    kernel_rmfs.block_bitmap_dirty = kernel_calloc(
        (size_t)kernel_rmfs.block_bitmap_blocks, 1);
    kernel_rmfs.metadata_cache = kernel_calloc(
        RMFS_METADATA_CACHE_ENTRIES, sizeof(RmfsMetadataCacheEntry));
    if (kernel_rmfs.inode_bitmap == NULL || kernel_rmfs.block_bitmap == NULL ||
        kernel_rmfs.inode_bitmap_dirty == NULL ||
        kernel_rmfs.block_bitmap_dirty == NULL ||
        kernel_rmfs.metadata_cache == NULL) {
        kernel_free(kernel_rmfs.inode_bitmap);
        kernel_free(kernel_rmfs.block_bitmap);
        kernel_free(kernel_rmfs.inode_bitmap_dirty);
        kernel_free(kernel_rmfs.block_bitmap_dirty);
        kernel_free(kernel_rmfs.metadata_cache);
        kernel_rmfs.inode_bitmap = NULL;
        kernel_rmfs.block_bitmap = NULL;
        kernel_rmfs.inode_bitmap_dirty = NULL;
        kernel_rmfs.block_bitmap_dirty = NULL;
        kernel_rmfs.metadata_cache = NULL;
        return 0;
    }
    kernel_rmfs.mounted = 1;
    RmfsInode root;
    if (!rmfs_read_inode(kernel_rmfs.root_inode, &root) ||
        (root.mode & RMFS_MODE_TYPE_MASK) != RMFS_MODE_DIRECTORY) {
        kernel_rmfs.mounted = 0;
        kernel_free(kernel_rmfs.inode_bitmap);
        kernel_free(kernel_rmfs.block_bitmap);
        kernel_free(kernel_rmfs.inode_bitmap_dirty);
        kernel_free(kernel_rmfs.block_bitmap_dirty);
        kernel_free(kernel_rmfs.metadata_cache);
        kernel_rmfs.inode_bitmap = NULL;
        kernel_rmfs.block_bitmap = NULL;
        kernel_rmfs.inode_bitmap_dirty = NULL;
        kernel_rmfs.block_bitmap_dirty = NULL;
        kernel_rmfs.metadata_cache = NULL;
        return 0;
    }
    return 1;
}

int kernel_rmfs_file_size(const char *path, size_t *file_size)
{
    if (file_size == NULL) return 0;
    kernel_spin_lock(&kernel_rmfs.lock);
    uint64_t number;
    RmfsInode inode;
    int okay = rmfs_resolve(path, &number, &inode) &&
               (inode.mode & RMFS_MODE_TYPE_MASK) == RMFS_MODE_REGULAR &&
               inode.size <= KERNEL_SIZE_MAX;
    if (okay) *file_size = (size_t)inode.size;
    kernel_spin_unlock(&kernel_rmfs.lock);
    return okay;
}

static int rmfs_read_inode_range(const RmfsInode *inode,
                                 size_t offset,
                                 void *buffer,
                                 size_t capacity,
                                 size_t *read_size)
{
    *read_size = 0;
    if (capacity == 0 || offset >= inode->size) return 1;
    size_t amount = (size_t)(inode->size - offset);
    if (amount > capacity) amount = capacity;
    uint8_t *output = buffer;
    size_t copied = 0;
    uint8_t *block = kernel_rmfs.scratch;
    while (copied < amount) {
        uint64_t file_offset = (uint64_t)offset + copied;
        uint64_t logical_block = file_offset / RMFS_BLOCK_SIZE;
        size_t inside = (size_t)(file_offset % RMFS_BLOCK_SIZE);
        uint64_t physical = 0;
        for (uint32_t i = 0; i < inode->extent_count; ++i) {
            const RmfsExtent *extent = &inode->extents[i];
            if (logical_block >= extent->logical &&
                logical_block - extent->logical < extent->count) {
                physical = extent->physical + logical_block - extent->logical;
                break;
            }
        }
        if (physical == 0 || !rmfs_read_block(physical, block)) return 0;
        size_t chunk = RMFS_BLOCK_SIZE - inside;
        if (chunk > amount - copied) chunk = amount - copied;
        for (size_t i = 0; i < chunk; ++i) output[copied + i] = block[inside + i];
        copied += chunk;
    }
    *read_size = copied;
    return 1;
}

int kernel_rmfs_read_range(const char *path,
                           size_t offset,
                           void *buffer,
                           size_t capacity,
                           size_t *read_size)
{
    if (read_size == NULL || (buffer == NULL && capacity != 0)) return 0;
    kernel_spin_lock(&kernel_rmfs.lock);
    uint64_t number;
    RmfsInode inode;
    int okay = rmfs_resolve(path, &number, &inode) &&
               (inode.mode & RMFS_MODE_TYPE_MASK) == RMFS_MODE_REGULAR &&
               inode.size <= KERNEL_SIZE_MAX &&
               rmfs_read_inode_range(&inode, offset, buffer, capacity, read_size);
    kernel_spin_unlock(&kernel_rmfs.lock);
    return okay;
}

int kernel_rmfs_read_file(const char *path,
                          void *buffer,
                          size_t capacity,
                          size_t *file_size)
{
    if (file_size == NULL) return 0;
    size_t size;
    if (!kernel_rmfs_file_size(path, &size) || size > capacity) return 0;
    size_t read_size;
    if (!kernel_rmfs_read_range(path, 0, buffer, size, &read_size) ||
        read_size != size) return 0;
    *file_size = size;
    return 1;
}

static int rmfs_write_transaction(const char *path,
                                  size_t write_offset,
                                  const void *buffer,
                                  size_t write_size,
                                  size_t final_size,
                                  int preserve)
{
    uint64_t parent_number;
    RmfsInode parent;
    const char *name;
    size_t name_length;
    RmfsDirectoryResult entry;
    int okay = rmfs_resolve_parent(path, &parent_number, &parent,
                                   &name, &name_length) &&
               rmfs_find_directory_entry(&parent, name, name_length, &entry);
    RmfsInode old_inode;
    uint64_t inode_number = 0;
    int creating = 0;
    if (okay && entry.found) {
        inode_number = entry.inode;
        okay = rmfs_read_inode(inode_number, &old_inode) &&
               (old_inode.mode & RMFS_MODE_TYPE_MASK) == RMFS_MODE_REGULAR;
    } else if (okay && !preserve) {
        creating = 1;
        old_inode.extent_count = 0;
        old_inode.size = 0;
    } else {
        okay = 0;
    }
    uint8_t *block_bitmap = kernel_rmfs.block_bitmap;
    uint8_t *inode_bitmap = kernel_rmfs.inode_bitmap;
    uint64_t old_blocks = old_inode.size / RMFS_BLOCK_SIZE +
                          (old_inode.size % RMFS_BLOCK_SIZE != 0);
    uint64_t final_blocks = (uint64_t)final_size / RMFS_BLOCK_SIZE +
                            ((uint64_t)final_size % RMFS_BLOCK_SIZE != 0);
    if (okay && preserve && final_size < old_inode.size) okay = 0;
    if (okay && preserve && write_size == 0) {
        return final_size == old_inode.size;
    }
    uint64_t replace_first = 0;
    uint64_t replace_end = final_blocks;
    if (okay && preserve) {
        uint64_t write_end = (uint64_t)write_offset + write_size;
        replace_first = (uint64_t)write_offset / RMFS_BLOCK_SIZE;
        replace_end = write_end / RMFS_BLOCK_SIZE +
                      (write_end % RMFS_BLOCK_SIZE != 0);
        if (final_blocks > old_blocks) {
            uint64_t exposed_first = old_blocks;
            if (old_blocks != 0 && old_inode.size % RMFS_BLOCK_SIZE != 0) {
                exposed_first = old_blocks - 1;
            }
            if (replace_first > exposed_first) replace_first = exposed_first;
            replace_end = final_blocks;
        }
    }
    uint64_t replacement_blocks = replace_end - replace_first;
    RmfsInode allocation;
    allocation.extent_count = 0;
    RmfsInode new_inode;
    new_inode.mode = creating
                         ? RMFS_MODE_REGULAR | UINT16_C(0644)
                         : old_inode.mode;
    new_inode.size = final_size;
    new_inode.parent = creating ? parent_number : old_inode.parent;
    new_inode.extent_count = 0;
    RmfsInode retired_inode;
    retired_inode.extent_count = 0;
    uint64_t directory_blocks_added = 0;
    if (okay) okay = block_bitmap != NULL && inode_bitmap != NULL;
    if (okay && replacement_blocks != 0) {
        okay = rmfs_allocate_extents(block_bitmap, replacement_blocks,
                                     &allocation);
    }
    if (okay && preserve) {
        uint64_t retired_end = replace_end < old_blocks
                                   ? replace_end : old_blocks;
        okay = rmfs_append_inode_range(&new_inode, &old_inode,
                                       0, replace_first) &&
               rmfs_append_allocated(&new_inode, &allocation,
                                     replace_first) &&
               rmfs_append_inode_range(&new_inode, &old_inode,
                                       replace_end, old_blocks) &&
               rmfs_append_inode_range(&retired_inode, &old_inode,
                                       replace_first, retired_end);
    } else if (okay) {
        okay = rmfs_append_allocated(&new_inode, &allocation, 0) &&
               rmfs_append_inode_range(&retired_inode, &old_inode,
                                       0, old_blocks);
    }
    if (okay && creating) {
        inode_number = rmfs_allocate_inode(inode_bitmap);
        okay = inode_number != 0;
    }
    if (okay && creating && !entry.free_found) {
        okay = rmfs_expand_directory(block_bitmap, &parent, &entry);
        if (okay) directory_blocks_added = 1;
    }
    int commit_started = 0;
    if (okay) {
        commit_started = 1;
        if (!rmfs_begin_group()) okay = 0;
    }
    const uint8_t *input = buffer;
    uint8_t *data_block = kernel_rmfs.scratch;
    for (uint64_t i = replace_first; okay && i < replace_end; ++i) {
        for (size_t byte = 0; byte < RMFS_BLOCK_SIZE; ++byte) data_block[byte] = 0;
        uint64_t block_start = i * RMFS_BLOCK_SIZE;
        uint64_t block_end = block_start + RMFS_BLOCK_SIZE;
        if (preserve && block_start < old_inode.size) {
            uint64_t old_physical = rmfs_inode_physical_block(&old_inode, i);
            if (old_physical == 0 ||
                !rmfs_read_block(old_physical, data_block)) okay = 0;
            if (okay && old_inode.size < block_end) {
                size_t first_zero = (size_t)(old_inode.size - block_start);
                for (size_t byte = first_zero; byte < RMFS_BLOCK_SIZE; ++byte) {
                    data_block[byte] = 0;
                }
            }
        }
        uint64_t write_end = (uint64_t)write_offset + write_size;
        uint64_t overlap_start = block_start > write_offset
                                     ? block_start : write_offset;
        uint64_t overlap_end = block_end < write_end ? block_end : write_end;
        if (okay && overlap_start < overlap_end) {
            size_t destination = (size_t)(overlap_start - block_start);
            size_t source = (size_t)(overlap_start - write_offset);
            size_t amount = (size_t)(overlap_end - overlap_start);
            for (size_t byte = 0; byte < amount; ++byte) {
                data_block[destination + byte] = input[source + byte];
            }
        }
        uint64_t new_physical = rmfs_inode_physical_block(&new_inode, i);
        if (okay && (new_physical == 0 ||
                     !rmfs_write_block(new_physical, data_block))) okay = 0;
    }
    if (okay) {
        okay = rmfs_mark_extent_bits(kernel_rmfs.block_bitmap_dirty,
                                     kernel_rmfs.block_bitmap_blocks,
                                     &allocation) &&
               (directory_blocks_added == 0 ||
                rmfs_mark_bitmap_bit(kernel_rmfs.block_bitmap_dirty,
                                     kernel_rmfs.block_bitmap_blocks,
                                     entry.free_block)) &&
               (!creating ||
                rmfs_mark_bitmap_bit(kernel_rmfs.inode_bitmap_dirty,
                                     kernel_rmfs.inode_bitmap_blocks,
                                     inode_number - 1)) &&
               rmfs_write_inode(inode_number, &new_inode);
    }
    if (okay && creating) {
        okay = rmfs_write_dirent(entry.free_block, entry.free_offset,
                                 inode_number,
                                 RMFS_MODE_REGULAR | UINT16_C(0644),
                                 name, name_length);
        uint64_t required_size = entry.logical_offset + RMFS_DIRENT_SIZE;
        if (okay && required_size > parent.size) {
            parent.size = required_size;
            okay = rmfs_write_inode(parent_number, &parent);
        }
    }
    if (okay) {
        uint64_t released = rmfs_release_extents(block_bitmap, &retired_inode);
        kernel_rmfs.free_blocks += released;
        kernel_rmfs.free_blocks -= replacement_blocks +
                                   directory_blocks_added;
        if (creating) --kernel_rmfs.free_inodes;
        ++kernel_rmfs.sequence;
        okay = rmfs_mark_extent_bits(kernel_rmfs.block_bitmap_dirty,
                                     kernel_rmfs.block_bitmap_blocks,
                                     &retired_inode);
        if (okay) {
            ++kernel_rmfs.pending_transactions;
            if (kernel_rmfs.pending_transactions >=
                RMFS_GROUP_COMMIT_LIMIT) {
                okay = rmfs_sync_locked();
            }
        }
    }
    if (!okay && !commit_started) {
        (void)rmfs_release_extents(block_bitmap, &allocation);
        if (directory_blocks_added != 0) {
            rmfs_bitmap_set(block_bitmap, entry.free_block, 0);
        }
        if (creating && inode_number != 0) {
            rmfs_bitmap_set(inode_bitmap, inode_number - 1, 0);
        }
    } else if (!okay) {
        /* An I/O failure after DIRTY may have persisted a partial commit. */
        kernel_rmfs.mounted = 0;
    }
    return okay;
}

int kernel_rmfs_write_file(const char *path,
                           const void *buffer,
                           size_t size)
{
    if (!kernel_rmfs.mounted || kernel_block_read_only() ||
        (buffer == NULL && size != 0)) return 0;
    kernel_spin_lock(&kernel_rmfs.lock);
    int okay = rmfs_write_transaction(path, 0, buffer, size, size, 0);
    kernel_spin_unlock(&kernel_rmfs.lock);
    return okay;
}

int kernel_rmfs_write_range(const char *path,
                            size_t offset,
                            const void *buffer,
                            size_t size,
                            size_t final_size)
{
    if (!kernel_rmfs.mounted || kernel_block_read_only() || path == NULL ||
        (buffer == NULL && size != 0) || offset > final_size ||
        size > final_size - offset) return 0;
    kernel_spin_lock(&kernel_rmfs.lock);
    int okay = rmfs_write_transaction(path, offset, buffer, size,
                                      final_size, 1);
    kernel_spin_unlock(&kernel_rmfs.lock);
    return okay;
}

int kernel_rmfs_sync(void)
{
    kernel_spin_lock(&kernel_rmfs.lock);
    int okay = rmfs_sync_locked();
    kernel_spin_unlock(&kernel_rmfs.lock);
    return okay;
}
