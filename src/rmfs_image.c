#include "rmfs_image.h"

#include "boot_format.h"
#include "rmfs_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *output, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) output[i] = (uint8_t)(value >> (i * 8));
}

static void put_u64(uint8_t *output, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) output[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t get_u16(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t get_u32(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= (uint64_t)input[i] << (i * 8);
    return value;
}

static int fail(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return 0;
}

static int seek_file(FILE *file, uint64_t offset)
{
#if defined(_WIN32)
    return _fseeki64(file, (long long)offset, SEEK_SET) == 0;
#else
    return offset <= (uint64_t)LONG_MAX && fseek(file, (long)offset, SEEK_SET) == 0;
#endif
}

static int write_bytes(FILE *file, uint64_t offset,
                       const void *data, size_t size)
{
    return seek_file(file, offset) && fwrite(data, 1, size, file) == size;
}

static int read_bytes(FILE *file, uint64_t offset, void *data, size_t size)
{
    return seek_file(file, offset) && fread(data, 1, size, file) == size;
}

static uint64_t block_offset(uint64_t start_lba, uint64_t block)
{
    return start_lba * UINT64_C(512) + block * RMFS_BLOCK_SIZE;
}

static void bitmap_set(uint8_t *bitmap, uint64_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(UINT8_C(1) << (bit % 8));
}

static int bitmap_get(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit / 8] & (uint8_t)(UINT8_C(1) << (bit % 8))) != 0;
}

static void make_extent(uint8_t *inode, uint32_t index,
                        uint64_t logical, uint64_t physical,
                        uint32_t count)
{
    uint8_t *extent = inode + RMFS_IN_EXTENTS +
                      (size_t)index * RMFS_EXTENT_SIZE;
    put_u64(extent + RMFS_EX_LOGICAL_BLOCK, logical);
    put_u64(extent + RMFS_EX_PHYSICAL_BLOCK, physical);
    put_u32(extent + RMFS_EX_BLOCK_COUNT, count);
    put_u32(extent + RMFS_EX_FLAGS, 0);
}

static void finish_inode(uint8_t inode[RMFS_INODE_SIZE])
{
    put_u32(inode + RMFS_IN_CHECKSUM, 0);
    put_u32(inode + RMFS_IN_CHECKSUM,
            cvm_crc32(inode, RMFS_INODE_SIZE));
}

static void make_inode(uint8_t inode[RMFS_INODE_SIZE], uint16_t mode,
                       uint64_t size, uint64_t parent,
                       uint64_t physical, uint32_t blocks)
{
    memset(inode, 0, RMFS_INODE_SIZE);
    put_u16(inode + RMFS_IN_MODE, mode);
    put_u32(inode + RMFS_IN_LINK_COUNT, 1);
    put_u32(inode + RMFS_IN_EXTENT_COUNT, 1);
    put_u64(inode + RMFS_IN_SIZE, size);
    put_u64(inode + RMFS_IN_GENERATION, 1);
    put_u64(inode + RMFS_IN_PARENT, parent);
    make_extent(inode, 0, 0, physical, blocks);
    finish_inode(inode);
}

static void make_dirent(uint8_t entry[RMFS_DIRENT_SIZE], uint64_t inode,
                        uint16_t mode, const char *name)
{
    size_t length = strlen(name);
    memset(entry, 0, RMFS_DIRENT_SIZE);
    put_u64(entry + RMFS_DE_INODE, inode);
    put_u16(entry + RMFS_DE_MODE, mode);
    put_u16(entry + RMFS_DE_NAME_LENGTH, (uint16_t)length);
    memcpy(entry + RMFS_DE_NAME, name, length);
    put_u32(entry + RMFS_DE_CHECKSUM, 0);
    put_u32(entry + RMFS_DE_CHECKSUM,
            cvm_crc32(entry, RMFS_DIRENT_SIZE));
}

static void finish_superblock(uint8_t block[RMFS_BLOCK_SIZE])
{
    put_u32(block + RMFS_SB_CHECKSUM, 0);
    put_u32(block + RMFS_SB_CHECKSUM, cvm_crc32(block, RMFS_BLOCK_SIZE));
}

uint64_t rmfs_image_recommended_inode_count(uint64_t total_blocks)
{
    uint64_t inode_count = total_blocks / RMFS_BLOCKS_PER_INODE;
    if (inode_count < RMFS_MIN_INODE_COUNT) inode_count = RMFS_MIN_INODE_COUNT;
    if (inode_count > RMFS_MAX_INODE_COUNT) inode_count = RMFS_MAX_INODE_COUNT;
    /* One 4 KiB inode-table block contains exactly 16 inodes. */
    return (inode_count + 15) & ~UINT64_C(15);
}

int rmfs_image_format(FILE *file,
                      uint64_t partition_start_lba,
                      uint64_t partition_sectors,
                      const uint8_t uuid[16],
                      const uint8_t *init_image,
                      size_t init_size,
                      RmfsImageInfo *result,
                      char *error,
                      size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (file == NULL || uuid == NULL || init_image == NULL || init_size == 0 ||
        partition_start_lba > UINT64_MAX / 512 ||
        partition_sectors < RMFS_SECTORS_PER_BLOCK * 128U) {
        return fail(error, error_size, "invalid RMFS format arguments");
    }
    uint64_t total_blocks = partition_sectors / RMFS_SECTORS_PER_BLOCK;
    uint64_t inode_count = rmfs_image_recommended_inode_count(total_blocks);
    uint64_t inode_bitmap_blocks =
        (inode_count + (uint64_t)RMFS_BLOCK_SIZE * 8 - 1) /
        ((uint64_t)RMFS_BLOCK_SIZE * 8);
    uint64_t block_bitmap_blocks =
        (total_blocks + (uint64_t)RMFS_BLOCK_SIZE * 8 - 1) /
        ((uint64_t)RMFS_BLOCK_SIZE * 8);
    uint64_t inode_table_blocks =
        (inode_count * RMFS_INODE_SIZE + RMFS_BLOCK_SIZE - 1) /
        RMFS_BLOCK_SIZE;
    uint64_t inode_bitmap_start = 1;
    uint64_t block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks;
    uint64_t inode_table_start = block_bitmap_start + block_bitmap_blocks;
    uint64_t data_start = inode_table_start + inode_table_blocks;
    uint64_t backup_block = total_blocks - 1;
    uint64_t init_blocks = ((uint64_t)init_size + RMFS_BLOCK_SIZE - 1) /
                           RMFS_BLOCK_SIZE;
    uint64_t root_block = data_start;
    uint64_t bin_block = root_block + 1;
    uint64_t init_block = bin_block + 1;
    if (init_blocks > UINT32_MAX || init_block > backup_block ||
        init_blocks > backup_block - init_block) {
        return fail(error, error_size, "RMFS partition is too small");
    }

    size_t inode_bitmap_size = (size_t)inode_bitmap_blocks * RMFS_BLOCK_SIZE;
    size_t block_bitmap_size = (size_t)block_bitmap_blocks * RMFS_BLOCK_SIZE;
    size_t inode_table_size = (size_t)inode_table_blocks * RMFS_BLOCK_SIZE;
    uint8_t *inode_bitmap = calloc(1, inode_bitmap_size);
    uint8_t *block_bitmap = calloc(1, block_bitmap_size);
    uint8_t *inode_table = calloc(1, inode_table_size);
    if (inode_bitmap == NULL || block_bitmap == NULL || inode_table == NULL) {
        free(inode_bitmap);
        free(block_bitmap);
        free(inode_table);
        return fail(error, error_size, "cannot allocate RMFS metadata");
    }
    for (uint64_t block = 0; block < data_start; ++block) {
        bitmap_set(block_bitmap, block);
    }
    bitmap_set(block_bitmap, root_block);
    bitmap_set(block_bitmap, bin_block);
    for (uint64_t block = 0; block < init_blocks; ++block) {
        bitmap_set(block_bitmap, init_block + block);
    }
    bitmap_set(block_bitmap, backup_block);
    bitmap_set(inode_bitmap, RMFS_ROOT_INODE - 1);
    bitmap_set(inode_bitmap, RMFS_BIN_INODE - 1);
    bitmap_set(inode_bitmap, RMFS_INIT_INODE - 1);

    make_inode(inode_table,
               RMFS_MODE_DIRECTORY | UINT16_C(0755),
               RMFS_DIRENT_SIZE, RMFS_ROOT_INODE, root_block, 1);
    make_inode(inode_table + RMFS_INODE_SIZE,
               RMFS_MODE_DIRECTORY | UINT16_C(0755),
               RMFS_DIRENT_SIZE, RMFS_ROOT_INODE, bin_block, 1);
    make_inode(inode_table + RMFS_INODE_SIZE * 2,
               RMFS_MODE_REGULAR | UINT16_C(0755),
               init_size, RMFS_BIN_INODE, init_block, (uint32_t)init_blocks);

    uint8_t superblock[RMFS_BLOCK_SIZE];
    memset(superblock, 0, sizeof(superblock));
    memcpy(superblock + RMFS_SB_MAGIC, RMFS_MAGIC, RMFS_MAGIC_SIZE);
    put_u16(superblock + RMFS_SB_VERSION_MAJOR, RMFS_VERSION_MAJOR);
    put_u16(superblock + RMFS_SB_VERSION_MINOR, RMFS_VERSION_MINOR);
    put_u32(superblock + RMFS_SB_BLOCK_SIZE, RMFS_BLOCK_SIZE);
    put_u32(superblock + RMFS_SB_INODE_SIZE, RMFS_INODE_SIZE);
    put_u32(superblock + RMFS_SB_STATE, RMFS_STATE_CLEAN);
    put_u64(superblock + RMFS_SB_TOTAL_BLOCKS, total_blocks);
    uint64_t used_blocks = data_start + 2 + init_blocks + 1;
    put_u64(superblock + RMFS_SB_FREE_BLOCKS, total_blocks - used_blocks);
    put_u64(superblock + RMFS_SB_INODE_COUNT, inode_count);
    put_u64(superblock + RMFS_SB_FREE_INODES, inode_count - 3);
    put_u64(superblock + RMFS_SB_ROOT_INODE, RMFS_ROOT_INODE);
    put_u64(superblock + RMFS_SB_INODE_BITMAP_START, inode_bitmap_start);
    put_u64(superblock + RMFS_SB_INODE_BITMAP_BLOCKS, inode_bitmap_blocks);
    put_u64(superblock + RMFS_SB_BLOCK_BITMAP_START, block_bitmap_start);
    put_u64(superblock + RMFS_SB_BLOCK_BITMAP_BLOCKS, block_bitmap_blocks);
    put_u64(superblock + RMFS_SB_INODE_TABLE_START, inode_table_start);
    put_u64(superblock + RMFS_SB_INODE_TABLE_BLOCKS, inode_table_blocks);
    put_u64(superblock + RMFS_SB_DATA_START, data_start);
    put_u64(superblock + RMFS_SB_BACKUP_BLOCK, backup_block);
    put_u64(superblock + RMFS_SB_SEQUENCE, 1);
    memcpy(superblock + RMFS_SB_UUID, uuid, 16);
    finish_superblock(superblock);

    uint8_t directory[RMFS_BLOCK_SIZE];
    memset(directory, 0, sizeof(directory));
    make_dirent(directory, RMFS_BIN_INODE,
                RMFS_MODE_DIRECTORY | UINT16_C(0755), "BIN");
    int okay = write_bytes(file, block_offset(partition_start_lba, 0),
                           superblock, sizeof(superblock)) &&
               write_bytes(file, block_offset(partition_start_lba,
                                               inode_bitmap_start),
                           inode_bitmap, inode_bitmap_size) &&
               write_bytes(file, block_offset(partition_start_lba,
                                               block_bitmap_start),
                           block_bitmap, block_bitmap_size) &&
               write_bytes(file, block_offset(partition_start_lba,
                                               inode_table_start),
                           inode_table, inode_table_size) &&
               write_bytes(file, block_offset(partition_start_lba, root_block),
                           directory, sizeof(directory));
    if (okay) {
        memset(directory, 0, sizeof(directory));
        make_dirent(directory, RMFS_INIT_INODE,
                    RMFS_MODE_REGULAR | UINT16_C(0755), "INIT.EXF");
        okay = write_bytes(file, block_offset(partition_start_lba, bin_block),
                           directory, sizeof(directory)) &&
               write_bytes(file, block_offset(partition_start_lba, init_block),
                           init_image, init_size) &&
               write_bytes(file, block_offset(partition_start_lba, backup_block),
                           superblock, sizeof(superblock));
    }
    free(inode_bitmap);
    free(block_bitmap);
    free(inode_table);
    if (!okay) return fail(error, error_size, "cannot write RMFS volume");
    if (result != NULL) {
        result->total_blocks = total_blocks;
        result->free_blocks = total_blocks - used_blocks;
        result->inode_count = inode_count;
        result->free_inodes = inode_count - 3;
        result->data_start_block = data_start;
        result->init_inode = RMFS_INIT_INODE;
        result->init_size = init_size;
        result->init_first_block = init_block;
    }
    return 1;
}

static int valid_checksum(uint8_t *data, size_t size, size_t offset)
{
    uint32_t stored = get_u32(data + offset);
    put_u32(data + offset, 0);
    uint32_t actual = cvm_crc32(data, size);
    put_u32(data + offset, stored);
    return stored == actual;
}

static int read_inode(FILE *file, uint64_t partition_start_lba,
                      uint64_t table_start, uint64_t inode_number,
                      uint8_t inode[RMFS_INODE_SIZE])
{
    uint64_t index = inode_number - 1;
    uint64_t offset = block_offset(partition_start_lba, table_start) +
                      index * RMFS_INODE_SIZE;
    return inode_number != 0 && read_bytes(file, offset, inode, RMFS_INODE_SIZE) &&
           valid_checksum(inode, RMFS_INODE_SIZE, RMFS_IN_CHECKSUM);
}

static int find_dirent(FILE *file, uint64_t partition_start_lba,
                       const uint8_t inode[RMFS_INODE_SIZE], const char *name,
                       uint64_t *inode_number)
{
    uint32_t extent_count = get_u32(inode + RMFS_IN_EXTENT_COUNT);
    uint64_t size = get_u64(inode + RMFS_IN_SIZE);
    size_t name_length = strlen(name);
    uint64_t scanned = 0;
    uint8_t block[RMFS_BLOCK_SIZE];
    for (uint32_t extent_index = 0;
         extent_index < extent_count && extent_index < RMFS_INLINE_EXTENTS;
         ++extent_index) {
        const uint8_t *extent = inode + RMFS_IN_EXTENTS +
                                (size_t)extent_index * RMFS_EXTENT_SIZE;
        uint64_t physical = get_u64(extent + RMFS_EX_PHYSICAL_BLOCK);
        uint32_t count = get_u32(extent + RMFS_EX_BLOCK_COUNT);
        for (uint32_t i = 0; i < count && scanned < size; ++i) {
            if (!read_bytes(file,
                            block_offset(partition_start_lba, physical + i),
                            block, sizeof(block))) return 0;
            for (size_t offset = 0;
                 offset + RMFS_DIRENT_SIZE <= sizeof(block) && scanned < size;
                 offset += RMFS_DIRENT_SIZE, scanned += RMFS_DIRENT_SIZE) {
                uint8_t *entry = block + offset;
                uint64_t candidate = get_u64(entry + RMFS_DE_INODE);
                if (candidate == 0) continue;
                if (!valid_checksum(entry, RMFS_DIRENT_SIZE,
                                    RMFS_DE_CHECKSUM)) return 0;
                if (get_u16(entry + RMFS_DE_NAME_LENGTH) == name_length &&
                    memcmp(entry + RMFS_DE_NAME, name, name_length) == 0) {
                    *inode_number = candidate;
                    return 1;
                }
            }
        }
    }
    return 0;
}

int rmfs_image_inspect(FILE *file,
                       uint64_t partition_start_lba,
                       uint64_t partition_sectors,
                       RmfsImageInfo *result,
                       uint8_t **init_output,
                       size_t *init_size_output,
                       char *error,
                       size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (init_output != NULL) *init_output = NULL;
    if (init_size_output != NULL) *init_size_output = 0;
    if (file == NULL || partition_sectors < RMFS_SECTORS_PER_BLOCK * 128U ||
        (init_output == NULL) != (init_size_output == NULL)) {
        return fail(error, error_size, "invalid RMFS inspect arguments");
    }
    uint8_t superblock[RMFS_BLOCK_SIZE];
    if (!read_bytes(file, block_offset(partition_start_lba, 0),
                    superblock, sizeof(superblock)) ||
        memcmp(superblock + RMFS_SB_MAGIC, RMFS_MAGIC, RMFS_MAGIC_SIZE) != 0 ||
        get_u16(superblock + RMFS_SB_VERSION_MAJOR) != RMFS_VERSION_MAJOR ||
        get_u32(superblock + RMFS_SB_BLOCK_SIZE) != RMFS_BLOCK_SIZE ||
        get_u32(superblock + RMFS_SB_INODE_SIZE) != RMFS_INODE_SIZE ||
        get_u32(superblock + RMFS_SB_STATE) != RMFS_STATE_CLEAN ||
        !valid_checksum(superblock, sizeof(superblock), RMFS_SB_CHECKSUM)) {
        return fail(error, error_size, "invalid RMFS superblock");
    }
    uint64_t total_blocks = get_u64(superblock + RMFS_SB_TOTAL_BLOCKS);
    uint64_t inode_count = get_u64(superblock + RMFS_SB_INODE_COUNT);
    uint64_t inode_bitmap = get_u64(superblock + RMFS_SB_INODE_BITMAP_START);
    uint64_t inode_bitmap_blocks =
        get_u64(superblock + RMFS_SB_INODE_BITMAP_BLOCKS);
    uint64_t block_bitmap = get_u64(superblock + RMFS_SB_BLOCK_BITMAP_START);
    uint64_t block_bitmap_blocks =
        get_u64(superblock + RMFS_SB_BLOCK_BITMAP_BLOCKS);
    uint64_t inode_table = get_u64(superblock + RMFS_SB_INODE_TABLE_START);
    uint64_t inode_table_blocks =
        get_u64(superblock + RMFS_SB_INODE_TABLE_BLOCKS);
    uint64_t data_start = get_u64(superblock + RMFS_SB_DATA_START);
    uint64_t backup_block = get_u64(superblock + RMFS_SB_BACKUP_BLOCK);
    uint64_t bitmap_bits = (uint64_t)RMFS_BLOCK_SIZE * 8;
    uint64_t expected_inode_bitmap =
        (inode_count + bitmap_bits - 1) / bitmap_bits;
    uint64_t expected_block_bitmap =
        (total_blocks + bitmap_bits - 1) / bitmap_bits;
    uint64_t expected_inode_table = (inode_count + 15) / 16;
    if (total_blocks != partition_sectors / RMFS_SECTORS_PER_BLOCK ||
        inode_count < RMFS_INIT_INODE || inode_count > RMFS_MAX_INODE_COUNT ||
        inode_bitmap != 1 || inode_bitmap_blocks != expected_inode_bitmap ||
        block_bitmap != inode_bitmap + expected_inode_bitmap ||
        block_bitmap_blocks != expected_block_bitmap ||
        inode_table != block_bitmap + expected_block_bitmap ||
        inode_table_blocks != expected_inode_table ||
        data_start != inode_table + expected_inode_table ||
        data_start >= backup_block ||
        backup_block != total_blocks - 1) {
        return fail(error, error_size, "invalid RMFS geometry");
    }
    uint8_t backup[RMFS_BLOCK_SIZE];
    if (!read_bytes(file, block_offset(partition_start_lba, backup_block),
                    backup, sizeof(backup)) ||
        memcmp(superblock, backup, sizeof(backup)) != 0) {
        return fail(error, error_size, "RMFS backup superblock mismatch");
    }
    uint8_t bitmap_block[RMFS_BLOCK_SIZE];
    if (!read_bytes(file, block_offset(partition_start_lba, inode_bitmap),
                    bitmap_block, sizeof(bitmap_block)) ||
        !bitmap_get(bitmap_block, RMFS_ROOT_INODE - 1) ||
        !bitmap_get(bitmap_block, RMFS_BIN_INODE - 1) ||
        !bitmap_get(bitmap_block, RMFS_INIT_INODE - 1) ||
        !read_bytes(file, block_offset(partition_start_lba, block_bitmap),
                    bitmap_block, sizeof(bitmap_block))) {
        return fail(error, error_size, "invalid RMFS allocation maps");
    }

    uint8_t root_inode[RMFS_INODE_SIZE];
    uint8_t bin_inode[RMFS_INODE_SIZE];
    uint8_t init_inode[RMFS_INODE_SIZE];
    uint64_t bin_number;
    uint64_t init_number;
    if (!read_inode(file, partition_start_lba, inode_table,
                    RMFS_ROOT_INODE, root_inode) ||
        (get_u16(root_inode + RMFS_IN_MODE) & RMFS_MODE_TYPE_MASK) !=
            RMFS_MODE_DIRECTORY ||
        !find_dirent(file, partition_start_lba, root_inode, "BIN", &bin_number) ||
        bin_number != RMFS_BIN_INODE ||
        !read_inode(file, partition_start_lba, inode_table,
                    bin_number, bin_inode) ||
        (get_u16(bin_inode + RMFS_IN_MODE) & RMFS_MODE_TYPE_MASK) !=
            RMFS_MODE_DIRECTORY ||
        !find_dirent(file, partition_start_lba, bin_inode,
                     "INIT.EXF", &init_number) ||
        init_number != RMFS_INIT_INODE ||
        !read_inode(file, partition_start_lba, inode_table,
                    init_number, init_inode) ||
        (get_u16(init_inode + RMFS_IN_MODE) & RMFS_MODE_TYPE_MASK) !=
            RMFS_MODE_REGULAR) {
        return fail(error, error_size, "RMFS /BIN/INIT.EXF is invalid");
    }
    uint64_t image_size64 = get_u64(init_inode + RMFS_IN_SIZE);
    uint32_t extent_count = get_u32(init_inode + RMFS_IN_EXTENT_COUNT);
    if (image_size64 == 0 || image_size64 > SIZE_MAX || extent_count == 0 ||
        extent_count > RMFS_INLINE_EXTENTS) {
        return fail(error, error_size, "invalid RMFS init inode");
    }
    size_t image_size = (size_t)image_size64;
    uint8_t *image = malloc(image_size);
    if (image == NULL) return fail(error, error_size, "cannot allocate init image");
    size_t copied = 0;
    for (uint32_t extent_index = 0;
         extent_index < extent_count && copied < image_size;
         ++extent_index) {
        const uint8_t *extent = init_inode + RMFS_IN_EXTENTS +
                                (size_t)extent_index * RMFS_EXTENT_SIZE;
        uint64_t physical = get_u64(extent + RMFS_EX_PHYSICAL_BLOCK);
        uint32_t count = get_u32(extent + RMFS_EX_BLOCK_COUNT);
        size_t bytes = (size_t)count * RMFS_BLOCK_SIZE;
        if (bytes > image_size - copied) bytes = image_size - copied;
        if (physical < data_start || physical >= backup_block || count == 0 ||
            (uint64_t)count > backup_block - physical ||
            !read_bytes(file, block_offset(partition_start_lba, physical),
                        image + copied, bytes)) {
            free(image);
            return fail(error, error_size, "cannot read RMFS init extents");
        }
        copied += bytes;
    }
    if (copied != image_size) {
        free(image);
        return fail(error, error_size, "RMFS init extents end early");
    }
    if (result != NULL) {
        result->total_blocks = total_blocks;
        result->free_blocks = get_u64(superblock + RMFS_SB_FREE_BLOCKS);
        result->inode_count = inode_count;
        result->free_inodes = get_u64(superblock + RMFS_SB_FREE_INODES);
        result->data_start_block = data_start;
        result->init_inode = init_number;
        result->init_size = image_size;
        result->init_first_block = get_u64(init_inode + RMFS_IN_EXTENTS +
                                            RMFS_EX_PHYSICAL_BLOCK);
    }
    if (init_output != NULL) {
        *init_output = image;
        *init_size_output = image_size;
    } else {
        free(image);
    }
    return 1;
}
