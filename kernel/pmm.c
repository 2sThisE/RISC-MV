#include "kernel_internal.h"

static const CvmBootInfo *pmm_boot_info;
static uint32_t pmm_memory_map_index;
static uintptr_t pmm_range_next;
static uintptr_t pmm_range_end;
static uintptr_t pmm_recycled_head;
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

static int usable_range_bounds(uint64_t base,
                               uint64_t length,
                               uintptr_t *first_page,
                               uintptr_t *end_page)
{
    if (first_page == NULL || end_page == NULL || length == 0 ||
        base > UINT64_MAX - length ||
        base > UINT64_MAX - KERNEL_PAGE_MASK) {
        return 0;
    }
    uint64_t end = (base + length) & KERNEL_PTE_ADDRESS_MASK;
    uint64_t page = (base + KERNEL_PAGE_MASK) & KERNEL_PTE_ADDRESS_MASK;
    if (page == 0) page = KERNEL_PAGE_SIZE;
    *first_page = (uintptr_t)page;
    *end_page = (uintptr_t)end;
    return 1;
}

static const CvmMemoryMapEntry *memory_map_entry(uint32_t index)
{
    if (pmm_boot_info == NULL || index >= pmm_boot_info->memory_map_count) {
        return NULL;
    }
    const uint8_t *entries = (const uint8_t *)pmm_boot_info +
                             pmm_boot_info->memory_map_offset;
    return (const CvmMemoryMapEntry *)(
        entries + (size_t)index * pmm_boot_info->memory_map_entry_size);
}

static int select_next_usable_range(void)
{
    while (pmm_memory_map_index < pmm_boot_info->memory_map_count) {
        const CvmMemoryMapEntry *entry =
            memory_map_entry(pmm_memory_map_index++);
        if (entry == NULL) return 0;
        if (entry->type != CVM_MEMORY_USABLE) continue;
        uintptr_t first;
        uintptr_t end;
        if (!usable_range_bounds(entry->base, entry->length, &first, &end)) {
            return 0;
        }
        if (first < end) {
            pmm_range_next = first;
            pmm_range_end = end;
            return 1;
        }
    }
    pmm_range_next = 0;
    pmm_range_end = 0;
    return 0;
}

static uintptr_t take_fresh_page(void)
{
    while (pmm_range_next >= pmm_range_end) {
        if (!select_next_usable_range()) return 0;
    }
    uintptr_t page = pmm_range_next;
    pmm_range_next += (uintptr_t)KERNEL_PAGE_SIZE;
    return page;
}

int kernel_pmm_init(const CvmBootInfo *info)
{
    if (info == NULL) return 1;
    pmm_boot_info = info;
    pmm_memory_map_index = 0;
    pmm_range_next = 0;
    pmm_range_end = 0;
    pmm_recycled_head = 0;
    pmm_free_count = 0;

    const uint8_t *entry_bytes =
        (const uint8_t *)info + info->memory_map_offset;
    for (uint32_t i = 0; i < info->memory_map_count; ++i) {
        const CvmMemoryMapEntry *entry =
            (const CvmMemoryMapEntry *)entry_bytes;
        if (entry->type == CVM_MEMORY_USABLE) {
            uintptr_t first;
            uintptr_t end;
            if (!usable_range_bounds(entry->base, entry->length,
                                     &first, &end) ||
                (uint64_t)end > info->ram_size) {
                return 1;
            }
            uint64_t pages = first < end
                                 ? ((uint64_t)end - (uint64_t)first) /
                                       KERNEL_PAGE_SIZE
                                 : 0;
            if (pmm_free_count > UINT64_MAX - pages) return 1;
            pmm_free_count += pages;
        }
        entry_bytes += info->memory_map_entry_size;
    }
    return pmm_free_count == 0 ? 1 : 0;
}

uintptr_t kernel_pmm_alloc_page(void)
{
    uintptr_t page;
    if (pmm_recycled_head != 0) {
        page = pmm_recycled_head;
        uint64_t *link = kernel_phys_to_virt(page);
        if (link == NULL) return 0;
        pmm_recycled_head = (uintptr_t)*link;
    } else {
        page = take_fresh_page();
    }
    if (page == 0) return 0;

    uint64_t *words = kernel_phys_to_virt(page);
    if (words == NULL) return 0;
    --pmm_free_count;
    for (size_t i = 0; i < KERNEL_PAGE_SIZE / sizeof(uint64_t); ++i) {
        words[i] = 0;
    }
    return page;
}

void kernel_pmm_free_page(uintptr_t page)
{
    if (page == 0 || (page & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        pmm_boot_info == NULL || pmm_boot_info->ram_size < KERNEL_PAGE_SIZE ||
        (uint64_t)page > pmm_boot_info->ram_size - KERNEL_PAGE_SIZE) {
        return;
    }
    uint64_t *slot = kernel_phys_to_virt(page);
    if (slot == NULL) return;
    *slot = (uint64_t)pmm_recycled_head;
    pmm_recycled_head = page;
    ++pmm_free_count;
}

uint64_t kernel_pmm_free_page_count(void)
{
    return pmm_free_count;
}

void kernel_pmm_release_range(uintptr_t base, uintptr_t size)
{
    if ((base & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        (size & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        (uint64_t)base > UINT64_MAX - (uint64_t)size ||
        pmm_boot_info == NULL ||
        (uint64_t)base + (uint64_t)size > pmm_boot_info->ram_size) {
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
