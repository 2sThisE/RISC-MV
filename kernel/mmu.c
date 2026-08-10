#include "kernel_internal.h"

#include <cvm/intrin.h>

static uintptr_t kernel_ptbr;

uintptr_t kernel_page_table_root(void)
{
    return kernel_ptbr;
}

int kernel_map_page(uintptr_t root, uintptr_t virtual_address,
                    uintptr_t physical_address, uint64_t flags)
{
    uint64_t *level2 = (uint64_t *)root;
    size_t index2 = (size_t)((virtual_address >> 30) & UINT64_C(0x1FF));
    uint64_t entry2 = level2[index2];
    if ((entry2 & KERNEL_PTE_VALID) == 0) {
        uintptr_t page = kernel_pmm_alloc_page();
        if (page == 0) return 1;
        entry2 = (uint64_t)page | KERNEL_PTE_VALID;
        level2[index2] = entry2;
    }

    uint64_t *level1 = (uint64_t *)(uintptr_t)
        (entry2 & KERNEL_PTE_ADDRESS_MASK);
    size_t index1 = (size_t)((virtual_address >> 21) & UINT64_C(0x1FF));
    uint64_t entry1 = level1[index1];
    if ((entry1 & KERNEL_PTE_VALID) == 0) {
        uintptr_t page = kernel_pmm_alloc_page();
        if (page == 0) return 1;
        entry1 = (uint64_t)page | KERNEL_PTE_VALID;
        level1[index1] = entry1;
    }

    uint64_t *level0 = (uint64_t *)(uintptr_t)
        (entry1 & KERNEL_PTE_ADDRESS_MASK);
    size_t index0 = (size_t)((virtual_address >> 12) & UINT64_C(0x1FF));
    level0[index0] = ((uint64_t)physical_address & KERNEL_PTE_ADDRESS_MASK) |
                     flags;
    return 0;
}

static int map_identity_range(uintptr_t root, uintptr_t start, uintptr_t end,
                              uint64_t flags)
{
    uintptr_t page = start & (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    if (end > (uintptr_t)UINT64_MAX - (uintptr_t)KERNEL_PAGE_MASK) return 1;
    uintptr_t limit = (end + (uintptr_t)KERNEL_PAGE_MASK) &
                      (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    while (page < limit) {
        if (kernel_map_page(root, page, page, flags) != 0) return 1;
        page += (uintptr_t)KERNEL_PAGE_SIZE;
    }
    return 0;
}

int kernel_bootstrap_mmu(void)
{
    kernel_ptbr = kernel_pmm_alloc_page();
    if (kernel_ptbr == 0) return 1;

    uintptr_t ram_end = (uintptr_t)
        (kernel_boot_info->ram_size & KERNEL_PTE_ADDRESS_MASK);
    for (uintptr_t page = (uintptr_t)KERNEL_PAGE_SIZE;
         page < ram_end; page += (uintptr_t)KERNEL_PAGE_SIZE) {
        if (kernel_map_page(kernel_ptbr, page, page,
                            KERNEL_PTE_VALID | KERNEL_PTE_READ |
                            KERNEL_PTE_WRITE) != 0) {
            return 1;
        }
    }

    if (map_identity_range(kernel_ptbr,
                           (uintptr_t)kernel_text_start,
                           (uintptr_t)kernel_text_end,
                           KERNEL_PTE_VALID | KERNEL_PTE_READ |
                           KERNEL_PTE_EXECUTE) != 0 ||
        map_identity_range(kernel_ptbr,
                           (uintptr_t)kernel_rodata_start,
                           (uintptr_t)kernel_rodata_end,
                           KERNEL_PTE_VALID | KERNEL_PTE_READ) != 0 ||
        map_identity_range(kernel_ptbr,
                           (uintptr_t)kernel_data_start,
                           (uintptr_t)kernel_data_end,
                           KERNEL_PTE_VALID | KERNEL_PTE_READ |
                           KERNEL_PTE_WRITE) != 0 ||
        map_identity_range(kernel_ptbr,
                           (uintptr_t)kernel_stack_bottom,
                           (uintptr_t)kernel_stack_top,
                           KERNEL_PTE_VALID | KERNEL_PTE_READ |
                           KERNEL_PTE_WRITE) != 0 ||
        kernel_map_page(kernel_ptbr, (uintptr_t)KERNEL_UART_ALIAS,
                        kernel_uart_address,
                        KERNEL_PTE_VALID | KERNEL_PTE_READ |
                        KERNEL_PTE_WRITE) != 0) {
        return 1;
    }

    kernel_uart_address = (uintptr_t)KERNEL_UART_ALIAS;
    cvm_set_ptbr((uint64_t)kernel_ptbr);
    cvm_mmu_on();
    return cvm_get_mmu() == UINT64_C(1) ? 0 : 1;
}
