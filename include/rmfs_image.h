#ifndef RISC_MV_RMFS_IMAGE_H
#define RISC_MV_RMFS_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t inode_count;
    uint64_t free_inodes;
    uint64_t data_start_block;
    uint64_t init_inode;
    uint64_t init_size;
    uint64_t init_first_block;
} RmfsImageInfo;

uint64_t rmfs_image_recommended_inode_count(uint64_t total_blocks);

int rmfs_image_format(FILE *file,
                      uint64_t partition_start_lba,
                      uint64_t partition_sectors,
                      const uint8_t uuid[16],
                      const uint8_t *init_image,
                      size_t init_size,
                      RmfsImageInfo *info,
                      char *error,
                      size_t error_size);

int rmfs_image_inspect(FILE *file,
                       uint64_t partition_start_lba,
                       uint64_t partition_sectors,
                       RmfsImageInfo *info,
                       uint8_t **init_image,
                       size_t *init_size,
                       char *error,
                       size_t error_size);

#endif
