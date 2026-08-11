#include "boot_format.h"

#include <stdio.h>
#include <string.h>

enum {
    KERNEL_HEADER_CRC_OFFSET = 0x58,
    KERNEL_PAYLOAD_CRC_OFFSET = 0x5C,
    BOOTINFO_CHECKSUM_OFFSET = 0xF0
};

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

static int add_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (right > UINT64_MAX - left) {
        return 0;
    }
    *result = left + right;
    return 1;
}

static int multiply_size(size_t left, size_t right, size_t *result)
{
    if (left != 0 && right > SIZE_MAX / left) {
        return 0;
    }
    *result = left * right;
    return 1;
}

static int is_power_of_two_u64(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static int all_zero(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (data[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static CvmBootFormatStatus fail(CvmBootFormatStatus status,
                                char *error,
                                size_t error_size,
                                const char *message)
{
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return status;
}

static uint32_t crc32_with_zero_range(const uint8_t *data,
                                      size_t size,
                                      size_t zero_offset,
                                      size_t zero_size)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = (i >= zero_offset && i - zero_offset < zero_size)
                           ? 0
                           : data[i];
        crc ^= byte;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xFFFFFFFF);
}

uint32_t cvm_crc32(const void *data, size_t size)
{
    if (data == NULL && size != 0) {
        return 0;
    }
    return crc32_with_zero_range(data, size, size, 0);
}

void cvm_kernel_header_encode(uint8_t output[CVM_KERNEL_HEADER_SIZE],
                              const CvmKernelHeader *header)
{
    memset(output, 0, CVM_KERNEL_HEADER_SIZE);
    memcpy(output + 0x00, header->magic, 8);
    write_u16_le(output + 0x08, header->format_major);
    write_u16_le(output + 0x0A, header->format_minor);
    write_u32_le(output + 0x0C, header->header_size);
    write_u64_le(output + 0x10, header->flags);
    write_u32_le(output + 0x18, header->isa_id);
    write_u32_le(output + 0x1C, header->isa_version);
    output[0x20] = header->address_bits;
    output[0x21] = header->byte_order;
    write_u16_le(output + 0x22, header->segment_count);
    write_u32_le(output + 0x24, header->segment_entry_size);
    write_u64_le(output + 0x28, header->segment_table_offset);
    write_u64_le(output + 0x30, header->entry_physical_address);
    write_u64_le(output + 0x38, header->image_file_size);
    write_u64_le(output + 0x40, header->required_cpu_features);
    memcpy(output + 0x48, header->build_id, 16);
    write_u32_le(output + 0x58, header->header_crc32);
    write_u32_le(output + 0x5C, header->payload_crc32);
    write_u64_le(output + 0x60, header->entry_virtual_address);
    write_u64_le(output + 0x68, header->virtual_base);
    write_u64_le(output + 0x70, header->virtual_size);
    memcpy(output + 0x78, header->reserved, 8);
}

void cvm_kernel_header_decode(const uint8_t input[CVM_KERNEL_HEADER_SIZE],
                              CvmKernelHeader *header)
{
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, input + 0x00, 8);
    header->format_major = read_u16_le(input + 0x08);
    header->format_minor = read_u16_le(input + 0x0A);
    header->header_size = read_u32_le(input + 0x0C);
    header->flags = read_u64_le(input + 0x10);
    header->isa_id = read_u32_le(input + 0x18);
    header->isa_version = read_u32_le(input + 0x1C);
    header->address_bits = input[0x20];
    header->byte_order = input[0x21];
    header->segment_count = read_u16_le(input + 0x22);
    header->segment_entry_size = read_u32_le(input + 0x24);
    header->segment_table_offset = read_u64_le(input + 0x28);
    header->entry_physical_address = read_u64_le(input + 0x30);
    header->image_file_size = read_u64_le(input + 0x38);
    header->required_cpu_features = read_u64_le(input + 0x40);
    memcpy(header->build_id, input + 0x48, 16);
    header->header_crc32 = read_u32_le(input + 0x58);
    header->payload_crc32 = read_u32_le(input + 0x5C);
    header->entry_virtual_address = read_u64_le(input + 0x60);
    header->virtual_base = read_u64_le(input + 0x68);
    header->virtual_size = read_u64_le(input + 0x70);
    memcpy(header->reserved, input + 0x78, 8);
}

void cvm_kernel_segment_encode(uint8_t output[CVM_KERNEL_SEGMENT_SIZE],
                               const CvmKernelSegment *segment)
{
    memset(output, 0, CVM_KERNEL_SEGMENT_SIZE);
    write_u32_le(output + 0x00, segment->type);
    write_u32_le(output + 0x04, segment->flags);
    write_u64_le(output + 0x08, segment->file_offset);
    write_u64_le(output + 0x10, segment->load_address);
    write_u64_le(output + 0x18, segment->virtual_address);
    write_u64_le(output + 0x20, segment->file_size);
    write_u64_le(output + 0x28, segment->memory_size);
    write_u64_le(output + 0x30, segment->alignment);
    write_u64_le(output + 0x38, segment->reserved);
}

void cvm_kernel_segment_decode(const uint8_t input[CVM_KERNEL_SEGMENT_SIZE],
                               CvmKernelSegment *segment)
{
    memset(segment, 0, sizeof(*segment));
    segment->type = read_u32_le(input + 0x00);
    segment->flags = read_u32_le(input + 0x04);
    segment->file_offset = read_u64_le(input + 0x08);
    segment->load_address = read_u64_le(input + 0x10);
    segment->virtual_address = read_u64_le(input + 0x18);
    segment->file_size = read_u64_le(input + 0x20);
    segment->memory_size = read_u64_le(input + 0x28);
    segment->alignment = read_u64_le(input + 0x30);
    segment->reserved = read_u64_le(input + 0x38);
}

CvmBootFormatStatus cvm_kernel_image_finalize(uint8_t *image,
                                               size_t image_size)
{
    if (image == NULL || image_size < CVM_KERNEL_HEADER_SIZE) {
        return CVM_BOOT_FORMAT_INVALID_ARGUMENT;
    }

    CvmKernelHeader header;
    cvm_kernel_header_decode(image, &header);
    if (header.image_file_size != (uint64_t)image_size ||
        header.header_size != CVM_KERNEL_HEADER_SIZE) {
        return CVM_BOOT_FORMAT_BAD_LAYOUT;
    }

    header.header_crc32 = 0;
    header.payload_crc32 = cvm_crc32(image + CVM_KERNEL_HEADER_SIZE,
                                     image_size - CVM_KERNEL_HEADER_SIZE);
    cvm_kernel_header_encode(image, &header);
    header.header_crc32 = crc32_with_zero_range(image,
                                                CVM_KERNEL_HEADER_SIZE,
                                                KERNEL_HEADER_CRC_OFFSET,
                                                sizeof(uint32_t));
    cvm_kernel_header_encode(image, &header);
    return CVM_BOOT_FORMAT_OK;
}

static int ranges_overlap(uint64_t left_base,
                          uint64_t left_size,
                          uint64_t right_base,
                          uint64_t right_size)
{
    uint64_t left_end;
    uint64_t right_end;
    if (!add_u64(left_base, left_size, &left_end) ||
        !add_u64(right_base, right_size, &right_end)) {
        return 1;
    }
    return left_base < right_end && right_base < left_end;
}

CvmBootFormatStatus cvm_kernel_image_validate(const uint8_t *image,
                                               size_t image_size,
                                               CvmKernelHeader *result,
                                               char *error,
                                               size_t error_size)
{
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    if (image == NULL) {
        return fail(CVM_BOOT_FORMAT_INVALID_ARGUMENT,
                    error, error_size, "image is null");
    }
    if (image_size < CVM_KERNEL_HEADER_SIZE) {
        return fail(CVM_BOOT_FORMAT_TRUNCATED,
                    error, error_size, "EXF header is truncated");
    }

    CvmKernelHeader header;
    cvm_kernel_header_decode(image, &header);
    if (memcmp(header.magic, RISC_MV_EXF_MAGIC, 8) != 0) {
        return fail(CVM_BOOT_FORMAT_BAD_MAGIC,
                    error, error_size, "invalid RISC-MV EXF magic");
    }
    if (header.format_major != CVM_KERNEL_FORMAT_MAJOR ||
        header.format_minor > CVM_KERNEL_FORMAT_MINOR) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size, "unsupported EXF format version");
    }
    if (header.header_size != CVM_KERNEL_HEADER_SIZE ||
        header.segment_entry_size != CVM_KERNEL_SEGMENT_SIZE ||
        header.segment_count == 0 ||
        header.segment_count > CVM_KERNEL_MAX_SEGMENTS) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "invalid EXF table dimensions");
    }
    uint64_t known_flags = CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL;
    if ((header.flags & ~known_flags) != 0 ||
        header.isa_id != RARCH_M64_ISA_ID ||
        header.isa_version != CVM_ISA_VERSION ||
        header.address_bits != CVM_ADDRESS_BITS ||
        header.byte_order != CVM_BYTE_ORDER_LITTLE) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size, "EXF requires an unsupported RArchM64 ABI");
    }
    if (!all_zero(header.reserved, sizeof(header.reserved))) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size, "reserved EXF header bytes are set");
    }
    if (header.image_file_size != (uint64_t)image_size) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "EXF file size does not match header");
    }
    if (header.segment_table_offset < CVM_KERNEL_HEADER_SIZE ||
        (header.segment_table_offset & UINT64_C(7)) != 0 ||
        header.segment_table_offset > SIZE_MAX) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "invalid segment table offset");
    }

    size_t table_size;
    if (!multiply_size(header.segment_count,
                       CVM_KERNEL_SEGMENT_SIZE,
                       &table_size)) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "segment table size overflow");
    }
    size_t table_offset = (size_t)header.segment_table_offset;
    if (table_offset > image_size || table_size > image_size - table_offset) {
        return fail(CVM_BOOT_FORMAT_TRUNCATED,
                    error, error_size, "segment table is truncated");
    }
    size_t table_end = table_offset + table_size;

    uint32_t expected_header_crc = crc32_with_zero_range(
        image,
        CVM_KERNEL_HEADER_SIZE,
        KERNEL_HEADER_CRC_OFFSET,
        sizeof(uint32_t));
    if (header.header_crc32 != expected_header_crc) {
        return fail(CVM_BOOT_FORMAT_BAD_CHECKSUM,
                    error, error_size, "EXF header CRC32 mismatch");
    }
    uint32_t expected_payload_crc = cvm_crc32(
        image + CVM_KERNEL_HEADER_SIZE,
        image_size - CVM_KERNEL_HEADER_SIZE);
    if (header.payload_crc32 != expected_payload_crc) {
        return fail(CVM_BOOT_FORMAT_BAD_CHECKSUM,
                    error, error_size, "EXF payload CRC32 mismatch");
    }

    int relocatable =
        (header.flags & CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL) != 0;
    if (relocatable) {
        uint64_t virtual_end;
        if (header.virtual_size == 0 ||
            !add_u64(header.virtual_base,
                     header.virtual_size,
                     &virtual_end) ||
            header.entry_virtual_address < header.virtual_base ||
            header.entry_virtual_address >= virtual_end ||
            header.entry_physical_address !=
                header.entry_virtual_address - header.virtual_base) {
            return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                        error, error_size,
                        "invalid relocatable kernel address geometry");
        }
    } else if (header.entry_virtual_address != 0 ||
               header.virtual_base != 0 || header.virtual_size != 0) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size,
                    "fixed kernel contains virtual handoff fields");
    }

    int entry_found = 0;
    for (uint16_t i = 0; i < header.segment_count; ++i) {
        const uint8_t *encoded = image + table_offset +
                                 (size_t)i * CVM_KERNEL_SEGMENT_SIZE;
        CvmKernelSegment segment;
        cvm_kernel_segment_decode(encoded, &segment);
        if (segment.type != CVM_SEGMENT_LOAD ||
            (segment.flags & ~(uint32_t)(CVM_SEGMENT_READ |
                                         CVM_SEGMENT_WRITE |
                                         CVM_SEGMENT_EXECUTE)) != 0 ||
            (segment.flags & CVM_SEGMENT_READ) == 0 ||
            segment.reserved != 0) {
            return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                        error, error_size, "invalid load segment type or flags");
        }
        if (segment.memory_size == 0 ||
            segment.file_size > segment.memory_size ||
            !is_power_of_two_u64(segment.alignment) ||
            segment.load_address % segment.alignment != 0 ||
            segment.virtual_address % segment.alignment != 0) {
            return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                        error, error_size, "invalid load segment dimensions");
        }
        uint64_t load_end;
        if (!add_u64(segment.load_address,
                     segment.memory_size,
                     &load_end)) {
            return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                        error, error_size, "load segment address overflow");
        }
        (void)load_end;
        uint64_t virtual_end;
        if (!add_u64(segment.virtual_address,
                     segment.memory_size,
                     &virtual_end)) {
            return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                        error, error_size,
                        "virtual segment address overflow");
        }
        if (relocatable &&
            (segment.virtual_address < header.virtual_base ||
             segment.virtual_address - header.virtual_base !=
                 segment.load_address ||
             virtual_end - header.virtual_base > header.virtual_size)) {
            return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                        error, error_size,
                        "relocatable segment is outside the virtual image");
        }

        if (segment.file_size != 0) {
            uint64_t file_end;
            if (segment.file_offset < table_end ||
                !add_u64(segment.file_offset,
                         segment.file_size,
                         &file_end) ||
                file_end > image_size) {
                return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                            error, error_size, "load segment file range is invalid");
            }
        }

        for (uint16_t previous = 0; previous < i; ++previous) {
            CvmKernelSegment other;
            cvm_kernel_segment_decode(
                image + table_offset +
                    (size_t)previous * CVM_KERNEL_SEGMENT_SIZE,
                &other);
            if (ranges_overlap(segment.load_address,
                               segment.memory_size,
                               other.load_address,
                               other.memory_size)) {
                return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                            error, error_size, "load segments overlap in RAM");
            }
            if (ranges_overlap(segment.virtual_address,
                               segment.memory_size,
                               other.virtual_address,
                               other.memory_size)) {
                return fail(CVM_BOOT_FORMAT_BAD_SEGMENT,
                            error, error_size,
                            "load segments overlap virtually");
            }
        }

        uint64_t entry = relocatable ? header.entry_virtual_address
                                     : header.entry_physical_address;
        uint64_t segment_entry_base = relocatable ? segment.virtual_address
                                                  : segment.load_address;
        uint64_t segment_entry_end = relocatable ? virtual_end : load_end;
        if ((segment.flags & CVM_SEGMENT_EXECUTE) != 0 &&
            entry >= segment_entry_base && entry < segment_entry_end) {
            entry_found = 1;
        }
    }
    if (!entry_found) {
        return fail(CVM_BOOT_FORMAT_BAD_ENTRY,
                    error, error_size,
                    "kernel entry is outside executable segments");
    }

    if (result != NULL) {
        *result = header;
    }
    return CVM_BOOT_FORMAT_OK;
}

void cvm_boot_info_encode(uint8_t output[CVM_BOOTINFO_HEADER_SIZE],
                          const CvmBootInfo *info)
{
    memset(output, 0, CVM_BOOTINFO_HEADER_SIZE);
    memcpy(output + 0x00, info->magic, 8);
    write_u16_le(output + 0x08, info->version_major);
    write_u16_le(output + 0x0A, info->version_minor);
    write_u32_le(output + 0x0C, info->header_size);
    write_u32_le(output + 0x10, info->total_size);
    write_u32_le(output + 0x14, info->flags);
    write_u32_le(output + 0x18, info->boot_cpu_id);
    write_u32_le(output + 0x1C, info->reserved0);
    write_u64_le(output + 0x20, info->memory_map_offset);
    write_u32_le(output + 0x28, info->memory_map_count);
    write_u32_le(output + 0x2C, info->memory_map_entry_size);
    write_u64_le(output + 0x30, info->ram_base);
    write_u64_le(output + 0x38, info->ram_size);
    write_u64_le(output + 0x40, info->kernel_entry);
    write_u64_le(output + 0x48, info->kernel_physical_base);
    write_u64_le(output + 0x50, info->kernel_physical_size);
    write_u64_le(output + 0x58, info->initrd_base);
    write_u64_le(output + 0x60, info->initrd_size);
    write_u64_le(output + 0x68, info->command_line_offset);
    write_u32_le(output + 0x70, info->command_line_size);
    write_u32_le(output + 0x74, info->reserved1);
    write_u64_le(output + 0x78, info->vio_hub_base);
    write_u64_le(output + 0x80, info->system_info_base);
    write_u64_le(output + 0x88, info->system_control_base);
    write_u64_le(output + 0x90, info->irq_controller_base);
    write_u64_le(output + 0x98, info->core_control_base);
    write_u64_le(output + 0xA0, info->timer_base);
    write_u64_le(output + 0xA8, info->uart_base);
    write_u32_le(output + 0xB0, info->boot_device_slot);
    write_u32_le(output + 0xB4, info->boot_partition_index);
    write_u64_le(output + 0xB8, info->boot_partition_lba);
    write_u64_le(output + 0xC0, info->boot_partition_sectors);
    write_u32_le(output + 0xC8, info->page_size);
    write_u16_le(output + 0xCC, info->physical_address_bits);
    write_u16_le(output + 0xCE, info->virtual_address_bits);
    memcpy(output + 0xD0, info->random_seed, 32);
    write_u32_le(output + 0xF0, info->checksum);
    memcpy(output + 0xF4, info->reserved, 12);
}

void cvm_boot_info_decode(const uint8_t input[CVM_BOOTINFO_HEADER_SIZE],
                          CvmBootInfo *info)
{
    memset(info, 0, sizeof(*info));
    memcpy(info->magic, input + 0x00, 8);
    info->version_major = read_u16_le(input + 0x08);
    info->version_minor = read_u16_le(input + 0x0A);
    info->header_size = read_u32_le(input + 0x0C);
    info->total_size = read_u32_le(input + 0x10);
    info->flags = read_u32_le(input + 0x14);
    info->boot_cpu_id = read_u32_le(input + 0x18);
    info->reserved0 = read_u32_le(input + 0x1C);
    info->memory_map_offset = read_u64_le(input + 0x20);
    info->memory_map_count = read_u32_le(input + 0x28);
    info->memory_map_entry_size = read_u32_le(input + 0x2C);
    info->ram_base = read_u64_le(input + 0x30);
    info->ram_size = read_u64_le(input + 0x38);
    info->kernel_entry = read_u64_le(input + 0x40);
    info->kernel_physical_base = read_u64_le(input + 0x48);
    info->kernel_physical_size = read_u64_le(input + 0x50);
    info->initrd_base = read_u64_le(input + 0x58);
    info->initrd_size = read_u64_le(input + 0x60);
    info->command_line_offset = read_u64_le(input + 0x68);
    info->command_line_size = read_u32_le(input + 0x70);
    info->reserved1 = read_u32_le(input + 0x74);
    info->vio_hub_base = read_u64_le(input + 0x78);
    info->system_info_base = read_u64_le(input + 0x80);
    info->system_control_base = read_u64_le(input + 0x88);
    info->irq_controller_base = read_u64_le(input + 0x90);
    info->core_control_base = read_u64_le(input + 0x98);
    info->timer_base = read_u64_le(input + 0xA0);
    info->uart_base = read_u64_le(input + 0xA8);
    info->boot_device_slot = read_u32_le(input + 0xB0);
    info->boot_partition_index = read_u32_le(input + 0xB4);
    info->boot_partition_lba = read_u64_le(input + 0xB8);
    info->boot_partition_sectors = read_u64_le(input + 0xC0);
    info->page_size = read_u32_le(input + 0xC8);
    info->physical_address_bits = read_u16_le(input + 0xCC);
    info->virtual_address_bits = read_u16_le(input + 0xCE);
    memcpy(info->random_seed, input + 0xD0, 32);
    info->checksum = read_u32_le(input + 0xF0);
    memcpy(info->reserved, input + 0xF4, 12);
}

void cvm_boot_virtual_handoff_encode(
    uint8_t output[CVM_BOOT_VIRTUAL_HANDOFF_SIZE],
    const CvmBootVirtualHandoff *handoff)
{
    memset(output, 0, CVM_BOOT_VIRTUAL_HANDOFF_SIZE);
    memcpy(output + 0x00, handoff->magic, 8);
    write_u64_le(output + 0x08, handoff->kernel_virtual_base);
    write_u64_le(output + 0x10, handoff->kernel_virtual_size);
    write_u64_le(output + 0x18, handoff->initial_page_table_root);
    write_u64_le(output + 0x20, handoff->direct_map_base);
    write_u64_le(output + 0x28, handoff->direct_map_size);
    write_u64_le(output + 0x30, handoff->page_table_physical_base);
    write_u64_le(output + 0x38, handoff->page_table_physical_size);
}

void cvm_boot_virtual_handoff_decode(
    const uint8_t input[CVM_BOOT_VIRTUAL_HANDOFF_SIZE],
    CvmBootVirtualHandoff *handoff)
{
    memset(handoff, 0, sizeof(*handoff));
    memcpy(handoff->magic, input + 0x00, 8);
    handoff->kernel_virtual_base = read_u64_le(input + 0x08);
    handoff->kernel_virtual_size = read_u64_le(input + 0x10);
    handoff->initial_page_table_root = read_u64_le(input + 0x18);
    handoff->direct_map_base = read_u64_le(input + 0x20);
    handoff->direct_map_size = read_u64_le(input + 0x28);
    handoff->page_table_physical_base = read_u64_le(input + 0x30);
    handoff->page_table_physical_size = read_u64_le(input + 0x38);
}

void cvm_memory_map_entry_encode(uint8_t output[CVM_MEMORY_MAP_ENTRY_SIZE],
                                 const CvmMemoryMapEntry *entry)
{
    memset(output, 0, CVM_MEMORY_MAP_ENTRY_SIZE);
    write_u64_le(output + 0x00, entry->base);
    write_u64_le(output + 0x08, entry->length);
    write_u32_le(output + 0x10, entry->type);
    write_u32_le(output + 0x14, entry->attributes);
    write_u64_le(output + 0x18, entry->reserved);
}

void cvm_memory_map_entry_decode(const uint8_t input[CVM_MEMORY_MAP_ENTRY_SIZE],
                                 CvmMemoryMapEntry *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->base = read_u64_le(input + 0x00);
    entry->length = read_u64_le(input + 0x08);
    entry->type = read_u32_le(input + 0x10);
    entry->attributes = read_u32_le(input + 0x14);
    entry->reserved = read_u64_le(input + 0x18);
}

CvmBootFormatStatus cvm_boot_info_finalize(uint8_t *data, size_t data_size)
{
    if (data == NULL || data_size < CVM_BOOTINFO_HEADER_SIZE) {
        return CVM_BOOT_FORMAT_INVALID_ARGUMENT;
    }
    CvmBootInfo info;
    cvm_boot_info_decode(data, &info);
    if (info.total_size < CVM_BOOTINFO_HEADER_SIZE ||
        info.total_size > data_size) {
        return CVM_BOOT_FORMAT_BAD_LAYOUT;
    }
    write_u32_le(data + BOOTINFO_CHECKSUM_OFFSET, 0);
    uint32_t checksum = cvm_crc32(data, info.total_size);
    write_u32_le(data + BOOTINFO_CHECKSUM_OFFSET, checksum);
    return CVM_BOOT_FORMAT_OK;
}

CvmBootFormatStatus cvm_boot_info_validate(const uint8_t *data,
                                            size_t data_size,
                                            CvmBootInfo *result,
                                            char *error,
                                            size_t error_size)
{
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    if (data == NULL) {
        return fail(CVM_BOOT_FORMAT_INVALID_ARGUMENT,
                    error, error_size, "BootInfo is null");
    }
    if (data_size < CVM_BOOTINFO_HEADER_SIZE) {
        return fail(CVM_BOOT_FORMAT_TRUNCATED,
                    error, error_size, "BootInfo header is truncated");
    }

    CvmBootInfo info;
    cvm_boot_info_decode(data, &info);
    if (memcmp(info.magic, CVM_BOOTINFO_MAGIC, 8) != 0) {
        return fail(CVM_BOOT_FORMAT_BAD_MAGIC,
                    error, error_size, "invalid BootInfo magic");
    }
    if (info.version_major != CVM_BOOTINFO_VERSION_MAJOR ||
        info.version_minor > CVM_BOOTINFO_VERSION_MINOR) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size, "unsupported BootInfo version");
    }
    if (info.header_size != CVM_BOOTINFO_HEADER_SIZE ||
        info.total_size < CVM_BOOTINFO_HEADER_SIZE ||
        info.total_size > data_size) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "invalid BootInfo size");
    }
    uint32_t known_flags = CVM_BOOTINFO_FLAG_MMU_ENABLED |
                           CVM_BOOTINFO_FLAG_INITRD_PRESENT |
                           CVM_BOOTINFO_FLAG_COMMAND_LINE_PRESENT |
                           CVM_BOOTINFO_FLAG_RANDOM_SEED_VALID |
                           CVM_BOOTINFO_FLAG_GPT_BOOT |
                           CVM_BOOTINFO_FLAG_FALLBACK_BOOT;
    if ((info.flags & ~known_flags) != 0 || info.reserved0 != 0 ||
        info.reserved1 != 0 || !all_zero(info.reserved, sizeof(info.reserved))) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size, "reserved BootInfo fields are set");
    }
    uint32_t expected_checksum = crc32_with_zero_range(
        data,
        info.total_size,
        BOOTINFO_CHECKSUM_OFFSET,
        sizeof(uint32_t));
    if (info.checksum != expected_checksum) {
        return fail(CVM_BOOT_FORMAT_BAD_CHECKSUM,
                    error, error_size, "BootInfo CRC32 mismatch");
    }

    uint64_t ram_end;
    if (info.ram_size == 0 ||
        !add_u64(info.ram_base, info.ram_size, &ram_end)) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "invalid RAM range");
    }
    (void)ram_end;
    if (info.page_size != 4096 ||
        info.physical_address_bits == 0 ||
        info.physical_address_bits > 64 ||
        info.virtual_address_bits == 0 ||
        info.virtual_address_bits > 64) {
        return fail(CVM_BOOT_FORMAT_UNSUPPORTED,
                    error, error_size, "unsupported address geometry");
    }

    if ((info.flags & CVM_BOOTINFO_FLAG_MMU_ENABLED) != 0) {
        if (info.memory_map_offset < CVM_BOOTINFO_HEADER_SIZE +
                                         CVM_BOOT_VIRTUAL_HANDOFF_SIZE) {
            return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                        error, error_size,
                        "MMU handoff extension is missing");
        }
        CvmBootVirtualHandoff handoff;
        cvm_boot_virtual_handoff_decode(
            data + CVM_BOOTINFO_HEADER_SIZE,
            &handoff);
        uint64_t kernel_virtual_end;
        uint64_t kernel_physical_end;
        uint64_t direct_map_end;
        uint64_t page_table_end;
        uint64_t virtual_limit = info.virtual_address_bits == 64
                                     ? UINT64_MAX
                                     : UINT64_C(1) <<
                                           info.virtual_address_bits;
        if (memcmp(handoff.magic,
                   CVM_BOOT_VIRTUAL_HANDOFF_MAGIC,
                   sizeof(handoff.magic)) != 0 ||
            handoff.kernel_virtual_size == 0 ||
            handoff.kernel_virtual_base % info.page_size != 0 ||
            !add_u64(handoff.kernel_virtual_base,
                     handoff.kernel_virtual_size,
                     &kernel_virtual_end) ||
            kernel_virtual_end > virtual_limit ||
            info.kernel_entry < handoff.kernel_virtual_base ||
            info.kernel_entry >= kernel_virtual_end ||
            info.kernel_physical_size == 0 ||
            info.kernel_physical_base % info.page_size != 0 ||
            !add_u64(info.kernel_physical_base,
                     info.kernel_physical_size,
                     &kernel_physical_end) ||
            kernel_physical_end > ram_end ||
            handoff.initial_page_table_root % info.page_size != 0 ||
            handoff.direct_map_base % info.page_size != 0 ||
            handoff.direct_map_size < info.ram_size ||
            !add_u64(handoff.direct_map_base,
                     handoff.direct_map_size,
                     &direct_map_end) ||
            direct_map_end > virtual_limit ||
            handoff.page_table_physical_size == 0 ||
            handoff.page_table_physical_base % info.page_size != 0 ||
            handoff.page_table_physical_size % info.page_size != 0 ||
            !add_u64(handoff.page_table_physical_base,
                     handoff.page_table_physical_size,
                     &page_table_end) ||
            page_table_end > ram_end ||
            handoff.initial_page_table_root <
                handoff.page_table_physical_base ||
            handoff.initial_page_table_root >
                page_table_end - info.page_size) {
            return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                        error, error_size,
                        "invalid MMU handoff geometry");
        }
    }

    if (info.memory_map_count == 0 ||
        info.memory_map_entry_size != CVM_MEMORY_MAP_ENTRY_SIZE ||
        info.memory_map_offset < CVM_BOOTINFO_HEADER_SIZE ||
        info.memory_map_offset > SIZE_MAX) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "invalid memory map description");
    }
    size_t map_size;
    if (!multiply_size(info.memory_map_count,
                       CVM_MEMORY_MAP_ENTRY_SIZE,
                       &map_size)) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "memory map size overflow");
    }
    size_t map_offset = (size_t)info.memory_map_offset;
    if (map_offset > info.total_size ||
        map_size > info.total_size - map_offset) {
        return fail(CVM_BOOT_FORMAT_TRUNCATED,
                    error, error_size, "memory map is truncated");
    }

    uint64_t previous_end = 0;
    for (uint32_t i = 0; i < info.memory_map_count; ++i) {
        CvmMemoryMapEntry entry;
        cvm_memory_map_entry_decode(
            data + map_offset + (size_t)i * CVM_MEMORY_MAP_ENTRY_SIZE,
            &entry);
        uint64_t end;
        if (entry.length == 0 || entry.type < CVM_MEMORY_USABLE ||
            entry.type > CVM_MEMORY_MMIO || entry.reserved != 0 ||
            !add_u64(entry.base, entry.length, &end) ||
            (i != 0 && entry.base < previous_end)) {
            return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                        error, error_size, "invalid or overlapping memory map");
        }
        previous_end = end;
    }

    int has_initrd = (info.flags & CVM_BOOTINFO_FLAG_INITRD_PRESENT) != 0;
    if (has_initrd != (info.initrd_size != 0) ||
        (!has_initrd && info.initrd_base != 0)) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "inconsistent initrd description");
    }

    int has_command =
        (info.flags & CVM_BOOTINFO_FLAG_COMMAND_LINE_PRESENT) != 0;
    if (has_command) {
        if (info.command_line_size == 0 ||
            info.command_line_offset < CVM_BOOTINFO_HEADER_SIZE ||
            info.command_line_offset > info.total_size ||
            info.command_line_size >
                info.total_size - (size_t)info.command_line_offset ||
            data[(size_t)info.command_line_offset +
                 info.command_line_size - 1] != 0) {
            return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                        error, error_size, "invalid kernel command line");
        }
    } else if (info.command_line_offset != 0 || info.command_line_size != 0) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "unexpected kernel command line");
    }

    if ((info.flags & CVM_BOOTINFO_FLAG_GPT_BOOT) != 0 &&
        (info.boot_partition_index == 0 ||
         info.boot_partition_sectors == 0)) {
        return fail(CVM_BOOT_FORMAT_BAD_LAYOUT,
                    error, error_size, "invalid GPT boot partition");
    }

    if (result != NULL) {
        *result = info;
    }
    return CVM_BOOT_FORMAT_OK;
}

const char *cvm_boot_format_status_name(CvmBootFormatStatus status)
{
    switch (status) {
    case CVM_BOOT_FORMAT_OK:
        return "ok";
    case CVM_BOOT_FORMAT_INVALID_ARGUMENT:
        return "invalid argument";
    case CVM_BOOT_FORMAT_TRUNCATED:
        return "truncated";
    case CVM_BOOT_FORMAT_BAD_MAGIC:
        return "bad magic";
    case CVM_BOOT_FORMAT_UNSUPPORTED:
        return "unsupported";
    case CVM_BOOT_FORMAT_BAD_LAYOUT:
        return "bad layout";
    case CVM_BOOT_FORMAT_BAD_CHECKSUM:
        return "bad checksum";
    case CVM_BOOT_FORMAT_BAD_SEGMENT:
        return "bad segment";
    case CVM_BOOT_FORMAT_BAD_ENTRY:
        return "bad entry";
    default:
        return "unknown";
    }
}
