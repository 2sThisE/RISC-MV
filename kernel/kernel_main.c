#include "kernel_internal.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

CvmBootInfo *kernel_boot_info;
CvmBootVirtualHandoff kernel_virtual_handoff;
uintptr_t kernel_uart_address = KERNEL_UART_ALIAS;

static int bytes_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static uint32_t boot_info_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = i >= 0xF0 && i < 0xF4 ? 0 : data[i];
        crc ^= byte;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

void kernel_uart_puts(const char *text)
{
    while (*text != '\0') {
        cvm_mmio_write8(kernel_uart_address, (uint8_t)*text);
        ++text;
    }
}

void kernel_uart_put_hex64(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (unsigned int i = 0; i < 16; ++i) {
        unsigned int shift = 60U - i * 4U;
        uint8_t digit = (uint8_t)((value >> shift) & UINT64_C(0xF));
        cvm_mmio_write8(kernel_uart_address, (uint8_t)digits[digit]);
    }
}

static int validate_boot_info(CvmBootInfo *info, uint64_t handoff_magic)
{
    static const uint8_t expected_magic[8] = CVM_BOOTINFO_MAGIC;
    if (info == NULL || handoff_magic != CVM_BOOTINFO_HANDOFF_MAGIC ||
        !bytes_equal(info->magic, expected_magic, sizeof(expected_magic)) ||
        info->version_major != CVM_BOOTINFO_VERSION_MAJOR ||
        info->header_size != CVM_BOOTINFO_HEADER_SIZE ||
        info->total_size < CVM_BOOTINFO_HEADER_SIZE ||
        info->memory_map_entry_size != CVM_MEMORY_MAP_ENTRY_SIZE ||
        info->ram_base != 0 || info->page_size != KERNEL_PAGE_SIZE ||
        info->uart_base == 0 ||
        (info->flags & CVM_BOOTINFO_FLAG_MMU_ENABLED) == 0 ||
        info->memory_map_offset < CVM_BOOTINFO_HEADER_SIZE +
                                      CVM_BOOT_VIRTUAL_HANDOFF_SIZE ||
        cvm_get_mmu() != 1) {
        return 1;
    }

    uint64_t map_bytes = (uint64_t)info->memory_map_count *
                         (uint64_t)info->memory_map_entry_size;
    uint64_t ram_end = info->ram_base + info->ram_size;
    uint64_t info_address = (uint64_t)(uintptr_t)info;
    if (info->memory_map_offset > info->total_size ||
        map_bytes > (uint64_t)info->total_size - info->memory_map_offset ||
        info->ram_base > UINT64_MAX - info->ram_size ||
        info_address < info->ram_base ||
        info_address > UINT64_MAX - info->total_size ||
        info_address + info->total_size > ram_end) {
        return 1;
    }
    if (boot_info_crc32((const uint8_t *)info, info->total_size) !=
        info->checksum) {
        return 1;
    }

    const CvmBootVirtualHandoff *virtual_handoff =
        (const CvmBootVirtualHandoff *)
            ((const uint8_t *)info + CVM_BOOTINFO_HEADER_SIZE);
    static const uint8_t virtual_magic[8] =
        CVM_BOOT_VIRTUAL_HANDOFF_MAGIC;
    if (!bytes_equal(virtual_handoff->magic,
                     virtual_magic,
                     sizeof(virtual_magic)) ||
        virtual_handoff->kernel_virtual_base != KERNEL_VIRTUAL_BASE ||
        virtual_handoff->direct_map_base != KERNEL_DIRECT_MAP_BASE ||
        virtual_handoff->direct_map_size < info->ram_size ||
        virtual_handoff->initial_page_table_root != cvm_get_ptbr()) {
        return 1;
    }

    kernel_boot_info = info;
    for (size_t i = 0; i < sizeof(kernel_virtual_handoff.magic); ++i) {
        kernel_virtual_handoff.magic[i] = virtual_handoff->magic[i];
    }
    kernel_virtual_handoff.kernel_virtual_base =
        virtual_handoff->kernel_virtual_base;
    kernel_virtual_handoff.kernel_virtual_size =
        virtual_handoff->kernel_virtual_size;
    kernel_virtual_handoff.initial_page_table_root =
        virtual_handoff->initial_page_table_root;
    kernel_virtual_handoff.direct_map_base =
        virtual_handoff->direct_map_base;
    kernel_virtual_handoff.direct_map_size =
        virtual_handoff->direct_map_size;
    kernel_virtual_handoff.page_table_physical_base =
        virtual_handoff->page_table_physical_base;
    kernel_virtual_handoff.page_table_physical_size =
        virtual_handoff->page_table_physical_size;
    kernel_uart_address = KERNEL_UART_ALIAS;
    return 0;
}

static int fail(const char *message)
{
    kernel_uart_puts(message);
    return 1;
}

int kernel_main(CvmBootInfo *info, uint64_t handoff_magic,
                uint64_t boot_thread_id)
{
    (void)boot_thread_id;
    if (validate_boot_info(info, handoff_magic) != 0) {
        return fail("KERNEL ERROR: BootInfo\n");
    }
    kernel_uart_puts("KERNEL: BootInfo OK\n");

    if (kernel_pmm_init(info) != 0 || kernel_pmm_self_test() != 0) {
        return fail("KERNEL ERROR: PMM\n");
    }
    kernel_uart_puts("KERNEL: PMM OK\n");

    if (kernel_bootstrap_mmu() != 0) {
        return fail("KERNEL ERROR: MMU\n");
    }
    kernel_uart_puts("KERNEL: MMU ON\n");

    if (kernel_pmm_self_test() != 0) {
        return fail("KERNEL ERROR: runtime PMM\n");
    }
    if (kernel_heap_init() != 0 || kernel_heap_self_test() != 0) {
        return fail("KERNEL ERROR: heap\n");
    }
    kernel_uart_puts("KERNEL: HEAP OK\n");

    if (kernel_runtime_self_test() != 0) {
        return fail("KERNEL ERROR: runtime structures\n");
    }
    kernel_uart_puts("KERNEL: STRUCTURES OK\n");

    if (kernel_devices_init() != 0 || kernel_devices_self_test() != 0) {
        return fail("KERNEL ERROR: devices\n");
    }
    kernel_uart_puts("KERNEL: DEVICES OK\n");

    if (kernel_vfs_init() != 0 || kernel_vfs_self_test() != 0) {
        return fail("KERNEL ERROR: VFS\n");
    }
    kernel_uart_puts("KERNEL: VFS FAT32 RW OK\n");

    if (kernel_user_loader_self_test() != 0) {
        return fail("KERNEL ERROR: user loader\n");
    }
    kernel_uart_puts("KERNEL: USER VM OK\n");

    if (kernel_exception_init() != 0) {
        return fail("KERNEL ERROR: exception init\n");
    }
    kernel_uart_puts("KERNEL: VBR OK\n");

    if (kernel_syscall_self_test() != 0) {
        return fail("KERNEL ERROR: syscall/user-copy self-test\n");
    }
    kernel_uart_puts("KERNEL: SYSCALL DISPATCH OK\n");

    if (kernel_memory_protection_self_test() != 0) {
        return fail("KERNEL ERROR: memory protection self-test\n");
    }
    kernel_uart_puts("KERNEL: MEMORY PROTECTION OK\n");

    if (kernel_demand_page_self_test() != 0) {
        return fail("KERNEL ERROR: demand paging self-test\n");
    }
    if (kernel_scheduler_self_test() != 0) {
        return fail("KERNEL ERROR: scheduler setup\n");
    }
    return 42;
}
