#include "kernel_internal.h"

#include <cvm/intrin.h>

static uintptr_t kernel_ptbr;

uintptr_t kernel_page_table_root(void)
{
    return kernel_ptbr;
}

static int table_entry_valid(uint64_t entry)
{
    return (entry & KERNEL_PTE_VALID) != 0 &&
           (entry & (KERNEL_PTE_PERMISSION_MASK | KERNEL_PTE_USER)) == 0;
}

static int allocate_next_table(uint64_t *entry)
{
    if ((*entry & KERNEL_PTE_VALID) != 0) {
        return table_entry_valid(*entry) ? 0 : 1;
    }
    uintptr_t page = kernel_pmm_alloc_page();
    if (page == 0) return 1;
    *entry = (uint64_t)page | KERNEL_PTE_VALID;
    return 0;
}

static int kernel_map_leaf(uintptr_t root,
                           uintptr_t virtual_address,
                           uintptr_t physical_address,
                           uint64_t flags,
                           unsigned int leaf_shift)
{
    uint64_t leaf_size = UINT64_C(1) << leaf_shift;
    uint64_t leaf_mask = leaf_size - UINT64_C(1);
    if (((uint64_t)virtual_address & leaf_mask) != 0 ||
        ((uint64_t)physical_address & leaf_mask) != 0 ||
        (flags & KERNEL_PTE_VALID) == 0 ||
        (flags & KERNEL_PTE_PERMISSION_MASK) == 0 ||
        (flags & ~(KERNEL_PTE_VALID | KERNEL_PTE_PERMISSION_MASK |
                   KERNEL_PTE_USER)) != 0 ||
        ((flags & KERNEL_PTE_WRITE) != 0 &&
         (flags & KERNEL_PTE_READ) == 0)) {
        return 1;
    }

    uint64_t *level2 = kernel_phys_to_virt(root);
    if (level2 == NULL) return 1;
    size_t index2 = (size_t)((virtual_address >> 30) & UINT64_C(0x1FF));
    if (leaf_shift == KERNEL_GIGA_PAGE_SHIFT) {
        if ((level2[index2] & KERNEL_PTE_VALID) != 0) return 1;
        level2[index2] = (uint64_t)physical_address | flags;
        return 0;
    }
    if (allocate_next_table(&level2[index2]) != 0) return 1;

    uint64_t *level1 = kernel_phys_to_virt(
        (uintptr_t)(level2[index2] & KERNEL_PTE_ADDRESS_MASK));
    if (level1 == NULL) return 1;
    size_t index1 = (size_t)((virtual_address >> 21) & UINT64_C(0x1FF));
    if (leaf_shift == KERNEL_LARGE_PAGE_SHIFT) {
        if ((level1[index1] & KERNEL_PTE_VALID) != 0) return 1;
        level1[index1] = (uint64_t)physical_address | flags;
        return 0;
    }
    if (allocate_next_table(&level1[index1]) != 0) return 1;

    uint64_t *level0 = kernel_phys_to_virt(
        (uintptr_t)(level1[index1] & KERNEL_PTE_ADDRESS_MASK));
    if (level0 == NULL) return 1;
    size_t index0 = (size_t)((virtual_address >> 12) & UINT64_C(0x1FF));
    level0[index0] = ((uint64_t)physical_address & KERNEL_PTE_ADDRESS_MASK) |
                     flags;
    return 0;
}

int kernel_map_page(uintptr_t root, uintptr_t virtual_address,
                    uintptr_t physical_address, uint64_t flags)
{
    return kernel_map_leaf(root, virtual_address, physical_address, flags,
                           KERNEL_PAGE_SHIFT);
}

static int map_range(uintptr_t root,
                     uintptr_t virtual_start,
                     uintptr_t physical_start,
                     uintptr_t size,
                     uint64_t flags)
{
    if ((virtual_start & (uintptr_t)KERNEL_PAGE_MASK) !=
            (physical_start & (uintptr_t)KERNEL_PAGE_MASK) ||
        (uint64_t)virtual_start > UINT64_MAX - (uint64_t)size ||
        (uint64_t)physical_start > UINT64_MAX - (uint64_t)size) {
        return 1;
    }
    uintptr_t virtual_page = virtual_start &
                             (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    uintptr_t physical_page = physical_start &
                              (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    uintptr_t leading = virtual_start - virtual_page;
    if ((uint64_t)size > UINT64_MAX - (uint64_t)leading -
                             (uint64_t)KERNEL_PAGE_MASK) {
        return 1;
    }
    uintptr_t mapped_size = (size + leading + (uintptr_t)KERNEL_PAGE_MASK) &
                            (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    uintptr_t offset = 0;
    while (offset < mapped_size) {
        uintptr_t virtual_address = virtual_page + offset;
        uintptr_t physical_address = physical_page + offset;
        uintptr_t remaining = mapped_size - offset;
        unsigned int leaf_shift = KERNEL_PAGE_SHIFT;
        uintptr_t leaf_size = (uintptr_t)KERNEL_PAGE_SIZE;
        if ((((uint64_t)virtual_address | (uint64_t)physical_address) &
             KERNEL_GIGA_PAGE_MASK) == 0 &&
            remaining >= (uintptr_t)KERNEL_GIGA_PAGE_SIZE) {
            leaf_shift = KERNEL_GIGA_PAGE_SHIFT;
            leaf_size = (uintptr_t)KERNEL_GIGA_PAGE_SIZE;
        } else if ((((uint64_t)virtual_address |
                     (uint64_t)physical_address) &
                    KERNEL_LARGE_PAGE_MASK) == 0 &&
                   remaining >= (uintptr_t)KERNEL_LARGE_PAGE_SIZE) {
            leaf_shift = KERNEL_LARGE_PAGE_SHIFT;
            leaf_size = (uintptr_t)KERNEL_LARGE_PAGE_SIZE;
        }
        if (kernel_map_leaf(root, virtual_address, physical_address, flags,
                            leaf_shift) != 0) {
            return 1;
        }
        offset += leaf_size;
    }
    return 0;
}

static uintptr_t kernel_symbol_physical(const void *symbol)
{
    uintptr_t virtual_address = (uintptr_t)symbol;
    uintptr_t virtual_base =
        (uintptr_t)kernel_virtual_handoff.kernel_virtual_base;
    if (virtual_address < virtual_base ||
        virtual_address - virtual_base >=
            (uintptr_t)kernel_virtual_handoff.kernel_virtual_size) {
        return 0;
    }
    return (uintptr_t)kernel_boot_info->kernel_physical_base +
           (virtual_address - virtual_base);
}

static int map_kernel_range(uintptr_t root,
                            const void *start,
                            const void *end,
                            uint64_t flags)
{
    uintptr_t virtual_start = (uintptr_t)start;
    uintptr_t virtual_end = (uintptr_t)end;
    uintptr_t physical_start = kernel_symbol_physical(start);
    if (physical_start == 0 || virtual_end < virtual_start) return 1;
    return map_range(root,
                     virtual_start,
                     physical_start,
                     virtual_end - virtual_start,
                     flags);
}

int kernel_bootstrap_mmu(void)
{
    kernel_ptbr = kernel_pmm_alloc_page();
    if (kernel_ptbr == 0) return 1;

    uintptr_t ram_size = (uintptr_t)
        (kernel_boot_info->ram_size & KERNEL_PTE_ADDRESS_MASK);
    if (map_range(kernel_ptbr,
                  (uintptr_t)KERNEL_DIRECT_MAP_BASE,
                  0,
                  ram_size,
                  KERNEL_PTE_VALID | KERNEL_PTE_READ |
                      KERNEL_PTE_WRITE) != 0) {
        return 1;
    }

    if (map_kernel_range(kernel_ptbr,
                         kernel_text_start,
                         kernel_text_end,
                         KERNEL_PTE_VALID | KERNEL_PTE_READ |
                         KERNEL_PTE_EXECUTE) != 0 ||
        map_kernel_range(kernel_ptbr,
                         kernel_rodata_start,
                         kernel_rodata_end,
                         KERNEL_PTE_VALID | KERNEL_PTE_READ) != 0 ||
        map_kernel_range(kernel_ptbr,
                         kernel_data_start,
                         kernel_data_end,
                         KERNEL_PTE_VALID | KERNEL_PTE_READ |
                         KERNEL_PTE_WRITE) != 0 ||
        map_kernel_range(kernel_ptbr,
                         kernel_stack_bottom,
                         kernel_stack_top,
                         KERNEL_PTE_VALID | KERNEL_PTE_READ |
                         KERNEL_PTE_WRITE) != 0 ||
        map_range(kernel_ptbr,
                  (uintptr_t)kernel_boot_info,
                  (uintptr_t)kernel_boot_info,
                  kernel_boot_info->total_size,
                  KERNEL_PTE_VALID | KERNEL_PTE_READ) != 0 ||
        map_range(kernel_ptbr,
                  KERNEL_EXCEPTION_STACK_TOP - KERNEL_PAGE_SIZE,
                  KERNEL_EXCEPTION_STACK_TOP - KERNEL_PAGE_SIZE,
                  KERNEL_PAGE_SIZE,
                  KERNEL_PTE_VALID | KERNEL_PTE_READ |
                  KERNEL_PTE_WRITE) != 0 ||
        kernel_map_page(kernel_ptbr, (uintptr_t)KERNEL_UART_ALIAS,
                        (uintptr_t)kernel_boot_info->uart_base,
                        KERNEL_PTE_VALID | KERNEL_PTE_READ |
                        KERNEL_PTE_WRITE) != 0) {
        return 1;
    }

    uintptr_t initial_page_table_base = (uintptr_t)
        kernel_virtual_handoff.page_table_physical_base;
    uintptr_t initial_page_table_size = (uintptr_t)
        kernel_virtual_handoff.page_table_physical_size;
    cvm_set_ptbr((uint64_t)kernel_ptbr);
    if (cvm_get_mmu() != UINT64_C(1) ||
        cvm_get_ptbr() != (uint64_t)kernel_ptbr) {
        return 1;
    }
    kernel_pmm_release_range(initial_page_table_base,
                             initial_page_table_size);
    return 0;
}
