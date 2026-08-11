#include "boot_format.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *make_kernel_image(size_t *image_size)
{
    static const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
    *image_size = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE +
                  sizeof(payload);
    uint8_t *image = calloc(1, *image_size);
    assert(image != NULL);

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
    header.entry_physical_address = 0x1000;
    header.image_file_size = *image_size;
    cvm_kernel_header_encode(image, &header);

    CvmKernelSegment segment = {
        .type = CVM_SEGMENT_LOAD,
        .flags = CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE,
        .file_offset = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE,
        .load_address = 0x1000,
        .virtual_address = 0x1000,
        .file_size = sizeof(payload),
        .memory_size = 0x100,
        .alignment = 0x1000
    };
    cvm_kernel_segment_encode(image + CVM_KERNEL_HEADER_SIZE, &segment);
    memcpy(image + segment.file_offset, payload, sizeof(payload));
    assert(cvm_kernel_image_finalize(image, *image_size) ==
           CVM_BOOT_FORMAT_OK);
    return image;
}

static void test_crc32(void)
{
    static const char vector[] = "123456789";
    assert(cvm_crc32(vector, sizeof(vector) - 1) == UINT32_C(0xCBF43926));
}

static void test_kernel_image(void)
{
    size_t image_size;
    uint8_t *image = make_kernel_image(&image_size);
    CvmKernelHeader header;
    char error[128];
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     &header,
                                     error,
                                     sizeof(error)) == CVM_BOOT_FORMAT_OK);
    assert(header.entry_physical_address == 0x1000);
    assert(header.segment_count == 1);

    memcpy(image, "CVMKERN1", 8);
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     NULL,
                                     error,
                                     sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_MAGIC);
    memcpy(image, RISC_MV_EXF_MAGIC, 8);

    CvmKernelHeader valid_header = header;
    header.isa_id = UINT32_C(0x314D5643); /* legacy "CVM1" */
    cvm_kernel_header_encode(image, &header);
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     NULL,
                                     error,
                                     sizeof(error)) ==
           CVM_BOOT_FORMAT_UNSUPPORTED);
    cvm_kernel_header_encode(image, &valid_header);
    header = valid_header;

    CvmKernelSegment segment;
    cvm_kernel_segment_decode(image + header.segment_table_offset, &segment);
    assert(segment.file_size == 4);
    assert(segment.memory_size == 0x100);
    assert(segment.flags == (CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE));

    image[image_size - 1] ^= 0x80;
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     NULL,
                                     error,
                                     sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_CHECKSUM);
    image[image_size - 1] ^= 0x80;

    cvm_kernel_header_decode(image, &header);
    header.entry_physical_address = 0x2000;
    cvm_kernel_header_encode(image, &header);
    assert(cvm_kernel_image_finalize(image, image_size) == CVM_BOOT_FORMAT_OK);
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     NULL,
                                     error,
                                     sizeof(error)) == CVM_BOOT_FORMAT_BAD_ENTRY);
    free(image);
}

static void test_relocatable_kernel_image(void)
{
    const size_t image_size = CVM_KERNEL_HEADER_SIZE +
                              CVM_KERNEL_SEGMENT_SIZE + 4;
    uint8_t *image = calloc(1, image_size);
    assert(image != NULL);

    CvmKernelHeader header = {0};
    memcpy(header.magic, RISC_MV_EXF_MAGIC, 8);
    header.format_major = CVM_KERNEL_FORMAT_MAJOR;
    header.format_minor = CVM_KERNEL_FORMAT_MINOR;
    header.header_size = CVM_KERNEL_HEADER_SIZE;
    header.flags = CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL;
    header.isa_id = RARCH_M64_ISA_ID;
    header.isa_version = CVM_ISA_VERSION;
    header.address_bits = CVM_ADDRESS_BITS;
    header.byte_order = CVM_BYTE_ORDER_LITTLE;
    header.segment_count = 1;
    header.segment_entry_size = CVM_KERNEL_SEGMENT_SIZE;
    header.segment_table_offset = CVM_KERNEL_HEADER_SIZE;
    header.entry_physical_address = 0x10;
    header.entry_virtual_address = UINT64_C(0x40000010);
    header.virtual_base = UINT64_C(0x40000000);
    header.virtual_size = 0x1000;
    header.image_file_size = image_size;
    cvm_kernel_header_encode(image, &header);

    CvmKernelSegment segment = {
        .type = CVM_SEGMENT_LOAD,
        .flags = CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE,
        .file_offset = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE,
        .load_address = 0,
        .virtual_address = UINT64_C(0x40000000),
        .file_size = 4,
        .memory_size = 0x1000,
        .alignment = 0x1000
    };
    cvm_kernel_segment_encode(image + CVM_KERNEL_HEADER_SIZE, &segment);
    assert(cvm_kernel_image_finalize(image, image_size) ==
           CVM_BOOT_FORMAT_OK);
    char error[128];
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     NULL,
                                     error,
                                     sizeof(error)) == CVM_BOOT_FORMAT_OK);

    cvm_kernel_header_decode(image, &header);
    header.entry_virtual_address++;
    cvm_kernel_header_encode(image, &header);
    assert(cvm_kernel_image_finalize(image, image_size) ==
           CVM_BOOT_FORMAT_OK);
    assert(cvm_kernel_image_validate(image,
                                     image_size,
                                     NULL,
                                     error,
                                     sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_LAYOUT);
    free(image);
}

static void test_virtual_handoff_encoding(void)
{
    CvmBootVirtualHandoff input = {0};
    memcpy(input.magic, CVM_BOOT_VIRTUAL_HANDOFF_MAGIC, 8);
    input.kernel_virtual_base = UINT64_C(0x40000000);
    input.kernel_virtual_size = 0x12000;
    input.initial_page_table_root = 0x42000;
    input.direct_map_base = UINT64_C(0x100000000);
    input.direct_map_size = 0x100000;
    input.page_table_physical_base = 0x42000;
    input.page_table_physical_size = 0x8000;
    uint8_t encoded[CVM_BOOT_VIRTUAL_HANDOFF_SIZE];
    cvm_boot_virtual_handoff_encode(encoded, &input);
    CvmBootVirtualHandoff output;
    cvm_boot_virtual_handoff_decode(encoded, &output);
    assert(memcmp(output.magic, input.magic, 8) == 0);
    assert(output.kernel_virtual_base == input.kernel_virtual_base);
    assert(output.kernel_virtual_size == input.kernel_virtual_size);
    assert(output.initial_page_table_root == input.initial_page_table_root);
    assert(output.direct_map_base == input.direct_map_base);
    assert(output.direct_map_size == input.direct_map_size);
    assert(output.page_table_physical_base ==
           input.page_table_physical_base);
    assert(output.page_table_physical_size ==
           input.page_table_physical_size);
}

static void test_mmu_boot_info(void)
{
    const uint32_t map_count = 4;
    const size_t map_offset = CVM_BOOTINFO_HEADER_SIZE +
                              CVM_BOOT_VIRTUAL_HANDOFF_SIZE;
    const size_t total_size = map_offset +
                              map_count * CVM_MEMORY_MAP_ENTRY_SIZE;
    uint8_t *data = calloc(1, total_size);
    assert(data != NULL);

    CvmBootInfo info = {0};
    memcpy(info.magic, CVM_BOOTINFO_MAGIC, 8);
    info.version_major = CVM_BOOTINFO_VERSION_MAJOR;
    info.version_minor = CVM_BOOTINFO_VERSION_MINOR;
    info.header_size = CVM_BOOTINFO_HEADER_SIZE;
    info.total_size = (uint32_t)total_size;
    info.flags = CVM_BOOTINFO_FLAG_MMU_ENABLED;
    info.memory_map_offset = map_offset;
    info.memory_map_count = map_count;
    info.memory_map_entry_size = CVM_MEMORY_MAP_ENTRY_SIZE;
    info.ram_size = 0x10000;
    info.kernel_entry = UINT64_C(0x40000010);
    info.kernel_physical_base = 0x1000;
    info.kernel_physical_size = 0x2000;
    info.page_size = 4096;
    info.physical_address_bits = 64;
    info.virtual_address_bits = 39;
    cvm_boot_info_encode(data, &info);

    CvmBootVirtualHandoff handoff = {0};
    memcpy(handoff.magic, CVM_BOOT_VIRTUAL_HANDOFF_MAGIC, 8);
    handoff.kernel_virtual_base = UINT64_C(0x40000000);
    handoff.kernel_virtual_size = 0x2000;
    handoff.initial_page_table_root = 0x3000;
    handoff.direct_map_base = UINT64_C(0x100000000);
    handoff.direct_map_size = info.ram_size;
    handoff.page_table_physical_base = 0x3000;
    handoff.page_table_physical_size = 0x1000;
    cvm_boot_virtual_handoff_encode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);

    const CvmMemoryMapEntry entries[] = {
        {0, 0x1000, CVM_MEMORY_BOOTLOADER_RECLAIMABLE,
         CVM_MEMORY_CACHEABLE | CVM_MEMORY_WRITABLE, 0},
        {0x1000, 0x2000, CVM_MEMORY_KERNEL,
         CVM_MEMORY_CACHEABLE | CVM_MEMORY_EXECUTABLE, 0},
        {0x3000, 0x1000, CVM_MEMORY_BOOTLOADER_RECLAIMABLE,
         CVM_MEMORY_CACHEABLE | CVM_MEMORY_WRITABLE, 0},
        {0x4000, 0xC000, CVM_MEMORY_USABLE,
         CVM_MEMORY_CACHEABLE | CVM_MEMORY_WRITABLE, 0}
    };
    for (uint32_t i = 0; i < map_count; ++i) {
        cvm_memory_map_entry_encode(
            data + map_offset + (size_t)i * CVM_MEMORY_MAP_ENTRY_SIZE,
            &entries[i]);
    }
    assert(cvm_boot_info_finalize(data, total_size) == CVM_BOOT_FORMAT_OK);
    char error[128];
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) == CVM_BOOT_FORMAT_OK);

    cvm_boot_virtual_handoff_decode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);
    handoff.direct_map_size = 0xF000;
    cvm_boot_virtual_handoff_encode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);
    assert(cvm_boot_info_finalize(data, total_size) == CVM_BOOT_FORMAT_OK);
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_LAYOUT);

    handoff.direct_map_size = info.ram_size;
    handoff.initial_page_table_root = 0x3001;
    cvm_boot_virtual_handoff_encode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);
    assert(cvm_boot_info_finalize(data, total_size) == CVM_BOOT_FORMAT_OK);
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_LAYOUT);

    handoff.initial_page_table_root = 0x4000;
    cvm_boot_virtual_handoff_encode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);
    assert(cvm_boot_info_finalize(data, total_size) == CVM_BOOT_FORMAT_OK);
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_LAYOUT);

    handoff.initial_page_table_root = 0x3000;
    handoff.page_table_physical_size = 0;
    cvm_boot_virtual_handoff_encode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);
    assert(cvm_boot_info_finalize(data, total_size) == CVM_BOOT_FORMAT_OK);
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_LAYOUT);

    handoff.page_table_physical_size = 0x1000;
    cvm_boot_virtual_handoff_encode(data + CVM_BOOTINFO_HEADER_SIZE,
                                    &handoff);
    info.memory_map_offset = CVM_BOOTINFO_HEADER_SIZE;
    cvm_boot_info_encode(data, &info);
    assert(cvm_boot_info_finalize(data, total_size) == CVM_BOOT_FORMAT_OK);
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_LAYOUT);
    free(data);
}

static uint8_t *make_boot_info(size_t *total_size)
{
    static const char command_line[] = "root=cvm0p1";
    const uint32_t map_count = 3;
    size_t map_size = map_count * CVM_MEMORY_MAP_ENTRY_SIZE;
    size_t command_offset = CVM_BOOTINFO_HEADER_SIZE + map_size;
    *total_size = command_offset + sizeof(command_line);
    uint8_t *data = calloc(1, *total_size);
    assert(data != NULL);

    CvmBootInfo info;
    memset(&info, 0, sizeof(info));
    memcpy(info.magic, CVM_BOOTINFO_MAGIC, 8);
    info.version_major = CVM_BOOTINFO_VERSION_MAJOR;
    info.version_minor = CVM_BOOTINFO_VERSION_MINOR;
    info.header_size = CVM_BOOTINFO_HEADER_SIZE;
    info.total_size = (uint32_t)*total_size;
    info.flags = CVM_BOOTINFO_FLAG_COMMAND_LINE_PRESENT |
                 CVM_BOOTINFO_FLAG_GPT_BOOT;
    info.memory_map_offset = CVM_BOOTINFO_HEADER_SIZE;
    info.memory_map_count = map_count;
    info.memory_map_entry_size = CVM_MEMORY_MAP_ENTRY_SIZE;
    info.ram_base = 0;
    info.ram_size = 0x10000;
    info.kernel_entry = 0x1000;
    info.kernel_physical_base = 0x1000;
    info.kernel_physical_size = 0x2000;
    info.command_line_offset = command_offset;
    info.command_line_size = sizeof(command_line);
    info.boot_device_slot = 6;
    info.boot_partition_index = 1;
    info.boot_partition_lba = 2048;
    info.boot_partition_sectors = 4096;
    info.page_size = 4096;
    info.physical_address_bits = 64;
    info.virtual_address_bits = 39;
    cvm_boot_info_encode(data, &info);

    const CvmMemoryMapEntry entries[] = {
        {
            .base = 0,
            .length = 0x1000,
            .type = CVM_MEMORY_BOOTLOADER_RECLAIMABLE,
            .attributes = CVM_MEMORY_CACHEABLE | CVM_MEMORY_WRITABLE
        },
        {
            .base = 0x1000,
            .length = 0x2000,
            .type = CVM_MEMORY_KERNEL,
            .attributes = CVM_MEMORY_CACHEABLE | CVM_MEMORY_EXECUTABLE
        },
        {
            .base = 0x3000,
            .length = 0xD000,
            .type = CVM_MEMORY_USABLE,
            .attributes = CVM_MEMORY_CACHEABLE | CVM_MEMORY_WRITABLE
        }
    };
    for (uint32_t i = 0; i < map_count; ++i) {
        cvm_memory_map_entry_encode(
            data + CVM_BOOTINFO_HEADER_SIZE +
                (size_t)i * CVM_MEMORY_MAP_ENTRY_SIZE,
            &entries[i]);
    }
    memcpy(data + command_offset, command_line, sizeof(command_line));
    assert(cvm_boot_info_finalize(data, *total_size) == CVM_BOOT_FORMAT_OK);
    return data;
}

static void test_boot_info(void)
{
    size_t total_size;
    uint8_t *data = make_boot_info(&total_size);
    CvmBootInfo info;
    char error[128];
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  &info,
                                  error,
                                  sizeof(error)) == CVM_BOOT_FORMAT_OK);
    assert(info.boot_device_slot == 6);
    assert(info.memory_map_count == 3);
    assert(info.kernel_entry == 0x1000);
    assert(strcmp((const char *)(data + info.command_line_offset),
                  "root=cvm0p1") == 0);

    data[CVM_BOOTINFO_HEADER_SIZE + 3] ^= 1;
    assert(cvm_boot_info_validate(data,
                                  total_size,
                                  NULL,
                                  error,
                                  sizeof(error)) ==
           CVM_BOOT_FORMAT_BAD_CHECKSUM);
    free(data);
}

int test_boot_format(void)
{
    test_crc32();
    test_kernel_image();
    test_relocatable_kernel_image();
    test_virtual_handoff_encoding();
    test_mmu_boot_info();
    test_boot_info();
    return 0;
}
