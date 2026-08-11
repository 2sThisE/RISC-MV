#include "kernel_internal.h"
#include "kernel_runtime.h"
#include "isa.h"

#define USER_HEADER_CRC_OFFSET ((size_t)0x58)
#define USER_PAYLOAD_CRC_OFFSET ((size_t)0x5C)

static uint16_t user_read_u16(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t user_read_u32(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static uint64_t user_read_u64(const uint8_t *input)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= (uint64_t)input[i] << (i * 8);
    return value;
}

static void user_write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void user_write_u32(uint8_t *output, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) output[i] = (uint8_t)(value >> (i * 8));
}

static void user_write_u64(uint8_t *output, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) output[i] = (uint8_t)(value >> (i * 8));
}

static int user_bytes_equal(const uint8_t *left,
                            const uint8_t *right,
                            size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static int user_all_zero(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) return 0;
    }
    return 1;
}

static uint32_t user_crc32(const uint8_t *data,
                           size_t size,
                           size_t zero_offset,
                           size_t zero_size)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = i >= zero_offset && i - zero_offset < zero_size
                           ? 0 : data[i];
        crc ^= byte;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static int user_add_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (right > UINT64_MAX - left) return 0;
    *result = left + right;
    return 1;
}

static int user_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static const uint8_t *user_segment(const uint8_t *data,
                                   uint64_t table_offset,
                                   uint16_t index)
{
    return data + (size_t)table_offset +
           (size_t)index * CVM_KERNEL_SEGMENT_SIZE;
}

static int user_validate_image(const uint8_t *data,
                               size_t size,
                               uint16_t *segment_count,
                               uint64_t *table_offset,
                               uintptr_t *entry,
                               uintptr_t *image_base,
                               uintptr_t *image_end)
{
    static const uint8_t magic[8] = RISC_MV_EXF_MAGIC;
    if (data == NULL || size < CVM_KERNEL_HEADER_SIZE ||
        !user_bytes_equal(data, magic, sizeof(magic)) ||
        user_read_u16(data + 0x08) != CVM_KERNEL_FORMAT_MAJOR ||
        user_read_u16(data + 0x0A) > CVM_KERNEL_FORMAT_MINOR ||
        user_read_u32(data + 0x0C) != CVM_KERNEL_HEADER_SIZE ||
        user_read_u64(data + 0x10) != 0 ||
        user_read_u32(data + 0x18) != RARCH_M64_ISA_ID ||
        user_read_u32(data + 0x1C) != CVM_ISA_VERSION ||
        data[0x20] != CVM_ADDRESS_BITS ||
        data[0x21] != CVM_BYTE_ORDER_LITTLE ||
        user_read_u32(data + 0x24) != CVM_KERNEL_SEGMENT_SIZE ||
        user_read_u64(data + 0x38) != size ||
        user_read_u64(data + 0x40) != 0 ||
        !user_all_zero(data + 0x60, 0x20)) {
        return 0;
    }
    uint16_t count = user_read_u16(data + 0x22);
    uint64_t offset = user_read_u64(data + 0x28);
    if (count == 0 || count > CVM_KERNEL_MAX_SEGMENTS ||
        offset < CVM_KERNEL_HEADER_SIZE || (offset & 7) != 0 ||
        offset > size ||
        (uint64_t)count * CVM_KERNEL_SEGMENT_SIZE > size - offset ||
        user_read_u32(data + USER_HEADER_CRC_OFFSET) !=
            user_crc32(data,
                       CVM_KERNEL_HEADER_SIZE,
                       USER_HEADER_CRC_OFFSET,
                       sizeof(uint32_t)) ||
        user_read_u32(data + USER_PAYLOAD_CRC_OFFSET) !=
            user_crc32(data + CVM_KERNEL_HEADER_SIZE,
                       size - CVM_KERNEL_HEADER_SIZE,
                       size,
                       0)) {
        return 0;
    }

    uint64_t selected_entry = user_read_u64(data + 0x30);
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0;
    int entry_found = 0;
    uint64_t table_end = offset + (uint64_t)count * CVM_KERNEL_SEGMENT_SIZE;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *segment = user_segment(data, offset, i);
        uint32_t type = user_read_u32(segment + 0x00);
        uint32_t flags = user_read_u32(segment + 0x04);
        uint64_t file_offset = user_read_u64(segment + 0x08);
        uint64_t load_address = user_read_u64(segment + 0x10);
        uint64_t virtual_address = user_read_u64(segment + 0x18);
        uint64_t file_size = user_read_u64(segment + 0x20);
        uint64_t memory_size = user_read_u64(segment + 0x28);
        uint64_t alignment = user_read_u64(segment + 0x30);
        uint64_t memory_end;
        uint64_t file_end;
        if (type != CVM_SEGMENT_LOAD ||
            (flags & ~(uint32_t)(CVM_SEGMENT_READ | CVM_SEGMENT_WRITE |
                                 CVM_SEGMENT_EXECUTE)) != 0 ||
            (flags & CVM_SEGMENT_READ) == 0 ||
            ((flags & CVM_SEGMENT_WRITE) != 0 &&
             (flags & CVM_SEGMENT_EXECUTE) != 0) ||
            memory_size == 0 || file_size > memory_size ||
            !user_power_of_two(alignment) ||
            load_address != virtual_address ||
            (virtual_address & KERNEL_PAGE_MASK) != 0 ||
            virtual_address % alignment != 0 ||
            virtual_address < KERNEL_USER_IMAGE_BASE ||
            !user_add_u64(virtual_address, memory_size, &memory_end) ||
            memory_end > KERNEL_USER_IMAGE_LIMIT ||
            user_read_u64(segment + 0x38) != 0 ||
            (file_size != 0 &&
             (file_offset < table_end ||
              !user_add_u64(file_offset, file_size, &file_end) ||
              file_end > size))) {
            return 0;
        }
        for (uint16_t previous = 0; previous < i; ++previous) {
            const uint8_t *other = user_segment(data, offset, previous);
            uint64_t other_base = user_read_u64(other + 0x18);
            uint64_t other_end = other_base + user_read_u64(other + 0x28);
            if (virtual_address < other_end && other_base < memory_end) {
                return 0;
            }
        }
        if ((flags & CVM_SEGMENT_EXECUTE) != 0 &&
            selected_entry >= virtual_address &&
            selected_entry < memory_end) {
            entry_found = 1;
        }
        if (virtual_address < lowest) lowest = virtual_address;
        if (memory_end > highest) highest = memory_end;
    }
    if (!entry_found) return 0;
    *segment_count = count;
    *table_offset = offset;
    *entry = (uintptr_t)selected_entry;
    *image_base = (uintptr_t)lowest;
    *image_end = (uintptr_t)highest;
    return 1;
}

static uint64_t user_page_flags(uint32_t segment_flags)
{
    uint64_t flags = KERNEL_PTE_READ;
    if ((segment_flags & CVM_SEGMENT_WRITE) != 0) flags |= KERNEL_PTE_WRITE;
    if ((segment_flags & CVM_SEGMENT_EXECUTE) != 0) flags |= KERNEL_PTE_EXECUTE;
    return flags;
}

static int user_load_segment(KernelAddressSpace *space,
                             const uint8_t *image,
                             const uint8_t *segment)
{
    uint32_t flags = user_read_u32(segment + 0x04);
    uint64_t file_offset = user_read_u64(segment + 0x08);
    uint64_t virtual_address = user_read_u64(segment + 0x18);
    uint64_t file_size = user_read_u64(segment + 0x20);
    uint64_t memory_size = user_read_u64(segment + 0x28);
    uint64_t memory_end = virtual_address + memory_size;
    uint64_t mapped_end = (memory_end + KERNEL_PAGE_MASK) &
                          KERNEL_PTE_ADDRESS_MASK;
    for (uint64_t page = virtual_address; page < mapped_end;
         page += KERNEL_PAGE_SIZE) {
        uintptr_t physical;
        if (kernel_address_space_map_anonymous(
                space, (uintptr_t)page, user_page_flags(flags),
                &physical) != 0) {
            return 0;
        }
        uint8_t *destination = kernel_phys_to_virt(physical);
        if (destination == NULL) return 0;
        uint64_t page_end = page + KERNEL_PAGE_SIZE;
        uint64_t file_virtual_end = virtual_address + file_size;
        uint64_t copy_start = page > virtual_address ? page : virtual_address;
        uint64_t copy_end = page_end < file_virtual_end
                                ? page_end : file_virtual_end;
        if (copy_end > copy_start) {
            size_t source = (size_t)(file_offset +
                                     copy_start - virtual_address);
            size_t target = (size_t)(copy_start - page);
            size_t count = (size_t)(copy_end - copy_start);
            for (size_t i = 0; i < count; ++i) {
                destination[target + i] = image[source + i];
            }
        }
    }
    return 1;
}

int kernel_user_image_load(const uint8_t *data,
                           size_t size,
                           KernelUserImage *image)
{
    if (image == NULL) return 1;
    image->address_space = NULL;
    image->entry = 0;
    image->stack_pointer = 0;
    image->image_base = 0;
    image->image_end = 0;
    uint16_t count;
    uint64_t table_offset;
    if (!user_validate_image(data, size, &count, &table_offset,
                             &image->entry, &image->image_base,
                             &image->image_end)) {
        return 1;
    }

    KernelAddressSpace *space = kernel_address_space_create();
    if (space == NULL) return 1;
    image->address_space = space;
    for (uint16_t i = 0; i < count; ++i) {
        if (!user_load_segment(space, data,
                               user_segment(data, table_offset, i))) {
            kernel_user_image_destroy(image);
            return 1;
        }
    }
    uintptr_t stack_bottom = (uintptr_t)(KERNEL_USER_STACK_TOP -
                                          KERNEL_USER_STACK_SIZE);
    for (uintptr_t page = stack_bottom;
         page < (uintptr_t)KERNEL_USER_STACK_TOP;
         page += (uintptr_t)KERNEL_PAGE_SIZE) {
        uintptr_t physical;
        if (kernel_address_space_map_anonymous(
                space, page, KERNEL_PTE_READ | KERNEL_PTE_WRITE,
                &physical) != 0) {
            kernel_user_image_destroy(image);
            return 1;
        }
    }
    image->stack_pointer = (uintptr_t)KERNEL_USER_STACK_TOP;
    return 0;
}

void kernel_user_image_destroy(KernelUserImage *image)
{
    if (image == NULL) return;
    kernel_address_space_destroy(image->address_space);
    image->address_space = NULL;
    image->entry = 0;
    image->stack_pointer = 0;
    image->image_base = 0;
    image->image_end = 0;
}

static void user_finalize_test_image(uint8_t *data, size_t size)
{
    user_write_u32(data + USER_PAYLOAD_CRC_OFFSET,
                   user_crc32(data + CVM_KERNEL_HEADER_SIZE,
                              size - CVM_KERNEL_HEADER_SIZE,
                              size, 0));
    user_write_u32(data + USER_HEADER_CRC_OFFSET, 0);
    user_write_u32(data + USER_HEADER_CRC_OFFSET,
                   user_crc32(data, CVM_KERNEL_HEADER_SIZE,
                              USER_HEADER_CRC_OFFSET,
                              sizeof(uint32_t)));
}

static uint8_t *user_make_test_image(size_t *size)
{
    *size = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE + 16;
    uint8_t *data = kernel_calloc(1, *size);
    if (data == NULL) return NULL;
    static const uint8_t magic[8] = RISC_MV_EXF_MAGIC;
    for (size_t i = 0; i < 8; ++i) data[i] = magic[i];
    user_write_u16(data + 0x08, CVM_KERNEL_FORMAT_MAJOR);
    user_write_u16(data + 0x0A, CVM_KERNEL_FORMAT_MINOR);
    user_write_u32(data + 0x0C, CVM_KERNEL_HEADER_SIZE);
    user_write_u32(data + 0x18, RARCH_M64_ISA_ID);
    user_write_u32(data + 0x1C, CVM_ISA_VERSION);
    data[0x20] = CVM_ADDRESS_BITS;
    data[0x21] = CVM_BYTE_ORDER_LITTLE;
    user_write_u16(data + 0x22, 1);
    user_write_u32(data + 0x24, CVM_KERNEL_SEGMENT_SIZE);
    user_write_u64(data + 0x28, CVM_KERNEL_HEADER_SIZE);
    user_write_u64(data + 0x30, KERNEL_USER_IMAGE_BASE);
    user_write_u64(data + 0x38, *size);

    uint8_t *segment = data + CVM_KERNEL_HEADER_SIZE;
    user_write_u32(segment + 0x00, CVM_SEGMENT_LOAD);
    user_write_u32(segment + 0x04,
                   CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE);
    user_write_u64(segment + 0x08,
                   CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE);
    user_write_u64(segment + 0x10, KERNEL_USER_IMAGE_BASE);
    user_write_u64(segment + 0x18, KERNEL_USER_IMAGE_BASE);
    user_write_u64(segment + 0x20, 16);
    user_write_u64(segment + 0x28, KERNEL_PAGE_SIZE);
    user_write_u64(segment + 0x30, KERNEL_PAGE_SIZE);
    for (size_t i = 0; i < 16; ++i) {
        data[CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE + i] =
            (uint8_t)(0x80 + i);
    }
    user_finalize_test_image(data, *size);
    return data;
}

static void user_emit_u32(uint8_t *code, size_t *cursor, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) {
        code[(*cursor)++] = (uint8_t)(value >> (i * 8));
    }
}

static void user_emit_u64(uint8_t *code, size_t *cursor, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        code[(*cursor)++] = (uint8_t)(value >> (i * 8));
    }
}

static void user_emit_movi32(uint8_t *code, size_t *cursor,
                             uint8_t reg, uint32_t value)
{
    code[(*cursor)++] = OP_MOVI32U;
    code[(*cursor)++] = reg;
    user_emit_u32(code, cursor, value);
}

static void user_emit_movi64(uint8_t *code, size_t *cursor,
                             uint8_t reg, uint64_t value)
{
    code[(*cursor)++] = OP_MOVI64;
    code[(*cursor)++] = reg;
    user_emit_u64(code, cursor, value);
}

uint8_t *kernel_user_test_program_create(const char *message,
                                         uint32_t loop_count,
                                         size_t *size)
{
    if (message == NULL || size == NULL || loop_count == 0) return NULL;
    size_t message_size = 0;
    while (message[message_size] != '\0') {
        if (message_size == 255) return NULL;
        ++message_size;
    }

    uint8_t code[256];
    size_t cursor = 0;
    user_emit_movi32(code, &cursor, 8, loop_count);
    const uint64_t loop_address = KERNEL_USER_IMAGE_BASE + cursor;
    code[cursor++] = OP_ADDI32;
    code[cursor++] = 8;
    user_emit_u32(code, &cursor, UINT32_MAX);
    code[cursor++] = OP_CMPI32;
    code[cursor++] = 8;
    user_emit_u32(code, &cursor, 0);
    code[cursor++] = OP_JNZ;
    user_emit_u64(code, &cursor, loop_address);

    user_emit_movi32(code, &cursor, 0, 1); /* SYS_WRITE */
    user_emit_movi32(code, &cursor, 1, 1); /* stdout */
    const size_t pointer_immediate = cursor + 2;
    user_emit_movi64(code, &cursor, 2, 0);
    user_emit_movi32(code, &cursor, 3, (uint32_t)message_size);
    code[cursor++] = OP_SYSCALL;
    user_emit_movi32(code, &cursor, 0, 0); /* SYS_EXIT */
    user_emit_movi32(code, &cursor, 1, 0);
    code[cursor++] = OP_SYSCALL;
    code[cursor++] = OP_JUMP;
    user_emit_u64(code, &cursor, KERNEL_USER_IMAGE_BASE + cursor - 1);

    const size_t message_offset = cursor;
    if (cursor + message_size > sizeof(code)) return NULL;
    for (size_t i = 0; i < message_size; ++i) code[cursor++] = message[i];
    user_write_u64(code + pointer_immediate,
                   KERNEL_USER_IMAGE_BASE + message_offset);

    *size = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE + cursor;
    uint8_t *data = kernel_calloc(1, *size);
    if (data == NULL) return NULL;
    static const uint8_t magic[8] = RISC_MV_EXF_MAGIC;
    for (size_t i = 0; i < 8; ++i) data[i] = magic[i];
    user_write_u16(data + 0x08, CVM_KERNEL_FORMAT_MAJOR);
    user_write_u16(data + 0x0A, CVM_KERNEL_FORMAT_MINOR);
    user_write_u32(data + 0x0C, CVM_KERNEL_HEADER_SIZE);
    user_write_u32(data + 0x18, RARCH_M64_ISA_ID);
    user_write_u32(data + 0x1C, CVM_ISA_VERSION);
    data[0x20] = CVM_ADDRESS_BITS;
    data[0x21] = CVM_BYTE_ORDER_LITTLE;
    user_write_u16(data + 0x22, 1);
    user_write_u32(data + 0x24, CVM_KERNEL_SEGMENT_SIZE);
    user_write_u64(data + 0x28, CVM_KERNEL_HEADER_SIZE);
    user_write_u64(data + 0x30, KERNEL_USER_IMAGE_BASE);
    user_write_u64(data + 0x38, *size);

    uint8_t *segment = data + CVM_KERNEL_HEADER_SIZE;
    user_write_u32(segment + 0x00, CVM_SEGMENT_LOAD);
    user_write_u32(segment + 0x04,
                   CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE);
    user_write_u64(segment + 0x08,
                   CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE);
    user_write_u64(segment + 0x10, KERNEL_USER_IMAGE_BASE);
    user_write_u64(segment + 0x18, KERNEL_USER_IMAGE_BASE);
    user_write_u64(segment + 0x20, cursor);
    user_write_u64(segment + 0x28, KERNEL_PAGE_SIZE);
    user_write_u64(segment + 0x30, KERNEL_PAGE_SIZE);
    uint8_t *payload = segment + CVM_KERNEL_SEGMENT_SIZE;
    for (size_t i = 0; i < cursor; ++i) payload[i] = code[i];
    user_finalize_test_image(data, *size);
    return data;
}

int kernel_user_loader_self_test(void)
{
    size_t size;
    uint8_t *data = user_make_test_image(&size);
    if (data == NULL) return 1;
    KernelUserImage image;

    data[size - 1] ^= 1;
    if (kernel_user_image_load(data, size, &image) == 0) return 1;
    data[size - 1] ^= 1;

    uint8_t *segment = data + CVM_KERNEL_HEADER_SIZE;
    user_write_u32(segment + 0x04,
                   CVM_SEGMENT_READ | CVM_SEGMENT_WRITE |
                       CVM_SEGMENT_EXECUTE);
    user_finalize_test_image(data, size);
    if (kernel_user_image_load(data, size, &image) == 0) return 1;
    user_write_u32(segment + 0x04,
                   CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE);
    user_finalize_test_image(data, size);

    if (kernel_user_image_load(data, size, &image) != 0 ||
        image.entry != (uintptr_t)KERNEL_USER_IMAGE_BASE ||
        image.stack_pointer != (uintptr_t)KERNEL_USER_STACK_TOP ||
        kernel_address_space_root(image.address_space) == 0 ||
        kernel_address_space_root(image.address_space) ==
            kernel_page_table_root()) {
        return 1;
    }
    uintptr_t physical;
    if (!kernel_address_space_resolve(
            image.address_space, image.entry, KERNEL_PTE_EXECUTE,
            &physical) ||
        kernel_address_space_resolve(
            image.address_space, image.entry, KERNEL_PTE_WRITE,
            &physical) ||
        !kernel_address_space_resolve(
            image.address_space, image.stack_pointer - 8,
            KERNEL_PTE_WRITE, &physical) ||
        kernel_address_space_resolve(
            image.address_space, image.stack_pointer -
                                     KERNEL_USER_STACK_SIZE - 8,
            KERNEL_PTE_READ, &physical) ||
        kernel_address_space_resolve(
            image.address_space, (uintptr_t)KERNEL_VIRTUAL_BASE,
            KERNEL_PTE_EXECUTE, &physical)) {
        return 1;
    }
    if (!kernel_address_space_resolve(
            image.address_space, image.entry, KERNEL_PTE_READ,
            &physical)) {
        return 1;
    }
    const uint8_t *loaded = kernel_phys_to_virt(
        physical & (uintptr_t)KERNEL_PTE_ADDRESS_MASK);
    if (loaded == NULL) return 1;
    for (size_t i = 0; i < 16; ++i) {
        if (loaded[i] != (uint8_t)(0x80 + i)) return 1;
    }

    kernel_user_image_destroy(&image);
    kernel_free(data);
    return kernel_heap_validate();
}
