#include "kernel_internal.h"

static uintptr_t pmm_free_head;
static uint64_t pmm_free_count;

void *kernel_phys_to_virt(uintptr_t physical_address)
{
    if ((uint64_t)physical_address >= kernel_virtual_handoff.direct_map_size) {
        return NULL;
    }
    return (void *)(uintptr_t)(kernel_virtual_handoff.direct_map_base +
                               (uint64_t)physical_address);
}

uintptr_t kernel_virt_to_phys(const void *virtual_address)
{
    uint64_t address = (uint64_t)(uintptr_t)virtual_address;
    uint64_t base = kernel_virtual_handoff.direct_map_base;
    if (address < base || address - base >=
                              kernel_virtual_handoff.direct_map_size) {
        return 0;
    }
    return (uintptr_t)(address - base);
}

static int add_usable_range(uint64_t base, uint64_t length)
{
    if (length == 0 || base > UINT64_MAX - length) return 0;
    uint64_t end = (base + length) & KERNEL_PTE_ADDRESS_MASK;
    if (base > UINT64_MAX - KERNEL_PAGE_MASK) return 0;
    uint64_t page = (base + KERNEL_PAGE_MASK) & KERNEL_PTE_ADDRESS_MASK;

    while (page < end) {
        uint64_t *slot = kernel_phys_to_virt((uintptr_t)page);
        if (slot == NULL) return 0;
        *slot = (uint64_t)pmm_free_head;
        pmm_free_head = (uintptr_t)page;
        ++pmm_free_count;
        page += KERNEL_PAGE_SIZE;
    }
    return 1;
}

int kernel_pmm_init(const CvmBootInfo *info)
{
    pmm_free_head = 0;
    pmm_free_count = 0;

    const uint8_t *entry_bytes =
        (const uint8_t *)info + info->memory_map_offset;
    for (uint32_t i = 0; i < info->memory_map_count; ++i) {
        const CvmMemoryMapEntry *entry =
            (const CvmMemoryMapEntry *)entry_bytes;
        if (entry->type == CVM_MEMORY_USABLE &&
            !add_usable_range(entry->base, entry->length)) {
            return 1;
        }
        entry_bytes += info->memory_map_entry_size;
    }
    return pmm_free_head == 0 ? 1 : 0;
}

uintptr_t kernel_pmm_alloc_page(void)
{
    uintptr_t page = pmm_free_head;
    if (page == 0) return 0;

    uint64_t *words = kernel_phys_to_virt(page);
    if (words == NULL) return 0;
    pmm_free_head = (uintptr_t)*words;
    --pmm_free_count;
    for (size_t i = 0; i < KERNEL_PAGE_SIZE / sizeof(uint64_t); ++i) {
        words[i] = 0;
    }
    return page;
}

void kernel_pmm_free_page(uintptr_t page)
{
    uint64_t *slot = kernel_phys_to_virt(page);
    if (slot == NULL) return;
    *slot = (uint64_t)pmm_free_head;
    pmm_free_head = page;
    ++pmm_free_count;
}

void kernel_pmm_release_range(uintptr_t base, uintptr_t size)
{
    if ((base & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        (size & (uintptr_t)KERNEL_PAGE_MASK) != 0) {
        return;
    }
    for (uintptr_t offset = 0; offset < size;
         offset += (uintptr_t)KERNEL_PAGE_SIZE) {
        kernel_pmm_free_page(base + offset);
    }
}

int kernel_pmm_self_test(void)
{
    uintptr_t page = kernel_pmm_alloc_page();
    if (page == 0) return 1;

    const uint64_t pattern = UINT64_C(0xC0DEC0DE55AA33CC);
    uint64_t *word = kernel_phys_to_virt(page);
    if (word == NULL) return 1;
    *word = pattern;
    if (*word != pattern) {
        kernel_pmm_free_page(page);
        return 1;
    }

    kernel_pmm_free_page(page);
    uintptr_t reused = kernel_pmm_alloc_page();
    if (reused != page) return 1;
    kernel_pmm_free_page(reused);
    return 0;
}
