#include "kernel_internal.h"
#include "kernel_runtime.h"

#define ADDRESS_PAGE_TABLE UINT64_C(1)
#define ADDRESS_PAGE_USER UINT64_C(2)
#define ADDRESS_PAGE_EXTERNAL UINT64_C(3)

typedef struct {
    KernelListNode node;
    uintptr_t physical_address;
    uintptr_t virtual_address;
    uint64_t kind;
} KernelAddressPage;

struct KernelAddressSpace {
    uintptr_t root;
    uintptr_t level1;
    uint64_t owned_level0[8];
    KernelList pages;
    KernelSpinLock lock;
};

static KernelAddressPage *address_find_user_page(
    KernelAddressSpace *space, uintptr_t virtual_address);
static uint64_t *address_lookup_level0(KernelAddressSpace *space,
                                       uintptr_t virtual_address);

static KernelAddressPage *address_page_allocate(KernelAddressSpace *space,
                                                uintptr_t virtual_address,
                                                uint64_t kind)
{
    KernelAddressPage *record = kernel_malloc(sizeof(*record));
    if (record == NULL) return NULL;
    uintptr_t physical = kernel_pmm_alloc_page();
    if (physical == 0) {
        kernel_free(record);
        return NULL;
    }
    record->node.previous = NULL;
    record->node.next = NULL;
    record->physical_address = physical;
    record->virtual_address = virtual_address;
    record->kind = kind;
    kernel_list_push_back(&space->pages, &record->node);
    return record;
}

static int address_level0_owned(const KernelAddressSpace *space,
                                size_t index)
{
    return (space->owned_level0[index / 64] &
            (UINT64_C(1) << (index & 63))) != 0;
}

static void address_mark_level0_owned(KernelAddressSpace *space,
                                      size_t index)
{
    space->owned_level0[index / 64] |= UINT64_C(1) << (index & 63);
}

static uint64_t *address_private_level0(KernelAddressSpace *space,
                                        size_t index1)
{
    uint64_t *level1 = kernel_phys_to_virt(space->level1);
    if (level1 == NULL) return NULL;
    uint64_t old_entry = level1[index1];
    if (address_level0_owned(space, index1)) {
        return kernel_phys_to_virt(
            (uintptr_t)(old_entry & KERNEL_PTE_ADDRESS_MASK));
    }

    KernelAddressPage *record = address_page_allocate(
        space, 0, ADDRESS_PAGE_TABLE);
    if (record == NULL) return NULL;
    uint64_t *new_level0 = kernel_phys_to_virt(record->physical_address);
    if (new_level0 == NULL) return NULL;
    if ((old_entry & KERNEL_PTE_VALID) != 0) {
        if ((old_entry & (KERNEL_PTE_READ | KERNEL_PTE_WRITE |
                          KERNEL_PTE_EXECUTE)) != 0) {
            return NULL;
        }
        const uint64_t *old_level0 = kernel_phys_to_virt(
            (uintptr_t)(old_entry & KERNEL_PTE_ADDRESS_MASK));
        if (old_level0 == NULL) return NULL;
        for (size_t i = 0; i < 512; ++i) new_level0[i] = old_level0[i];
    }
    level1[index1] = (uint64_t)record->physical_address | KERNEL_PTE_VALID;
    address_mark_level0_owned(space, index1);
    return new_level0;
}

KernelAddressSpace *kernel_address_space_create(void)
{
    KernelAddressSpace *space = kernel_calloc(1, sizeof(*space));
    if (space == NULL) return NULL;
    kernel_list_init(&space->pages);
    kernel_spin_init(&space->lock);

    KernelAddressPage *root = address_page_allocate(
        space, 0, ADDRESS_PAGE_TABLE);
    KernelAddressPage *level1 = address_page_allocate(
        space, 0, ADDRESS_PAGE_TABLE);
    if (root == NULL || level1 == NULL) {
        kernel_address_space_destroy(space);
        return NULL;
    }
    space->root = root->physical_address;
    space->level1 = level1->physical_address;

    const uint64_t *kernel_root = kernel_phys_to_virt(
        kernel_page_table_root());
    uint64_t *new_root = kernel_phys_to_virt(space->root);
    uint64_t *new_level1 = kernel_phys_to_virt(space->level1);
    if (kernel_root == NULL || new_root == NULL || new_level1 == NULL) {
        kernel_address_space_destroy(space);
        return NULL;
    }

    for (size_t i = 1; i < 512; ++i) new_root[i] = kernel_root[i];
    uint64_t kernel_entry0 = kernel_root[0];
    if ((kernel_entry0 & KERNEL_PTE_VALID) != 0) {
        const uint64_t *kernel_level1 = kernel_phys_to_virt(
            (uintptr_t)(kernel_entry0 & KERNEL_PTE_ADDRESS_MASK));
        if (kernel_level1 == NULL) {
            kernel_address_space_destroy(space);
            return NULL;
        }
        for (size_t i = 0; i < 512; ++i) new_level1[i] = kernel_level1[i];
    }
    new_root[0] = (uint64_t)space->level1 | KERNEL_PTE_VALID;
    return space;
}

void kernel_address_space_destroy(KernelAddressSpace *space)
{
    if (space == NULL) return;
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&space->pages)) != NULL) {
        KernelAddressPage *record = (KernelAddressPage *)node;
        if (record->kind != ADDRESS_PAGE_EXTERNAL) {
            kernel_pmm_free_page(record->physical_address);
        }
        kernel_free(record);
    }
    kernel_free(space);
}

uintptr_t kernel_address_space_root(const KernelAddressSpace *space)
{
    return space != NULL ? space->root : 0;
}

int kernel_address_space_map_anonymous(KernelAddressSpace *space,
                                       uintptr_t virtual_address,
                                       uint64_t flags,
                                       uintptr_t *physical_address)
{
    if (space == NULL || physical_address == NULL ||
        ((uint64_t)virtual_address & KERNEL_PAGE_MASK) != 0 ||
        (uint64_t)virtual_address < KERNEL_USER_IMAGE_BASE ||
        (uint64_t)virtual_address >= KERNEL_USER_STACK_TOP ||
        (flags & ~(KERNEL_PTE_READ | KERNEL_PTE_WRITE |
                   KERNEL_PTE_EXECUTE)) != 0 ||
        (flags & KERNEL_PTE_READ) == 0 ||
        ((flags & KERNEL_PTE_WRITE) != 0 &&
         (flags & KERNEL_PTE_EXECUTE) != 0)) {
        return 1;
    }

    kernel_spin_lock(&space->lock);
    size_t index2 = (size_t)(((uint64_t)virtual_address >> 30) & 0x1FF);
    size_t index1 = (size_t)(((uint64_t)virtual_address >> 21) & 0x1FF);
    size_t index0 = (size_t)(((uint64_t)virtual_address >> 12) & 0x1FF);
    if (index2 != 0) {
        kernel_spin_unlock(&space->lock);
        return 1;
    }
    uint64_t *level0 = address_private_level0(space, index1);
    if (level0 == NULL || (level0[index0] & KERNEL_PTE_VALID) != 0) {
        kernel_spin_unlock(&space->lock);
        return 1;
    }
    KernelAddressPage *page = address_page_allocate(
        space, virtual_address, ADDRESS_PAGE_USER);
    if (page == NULL) {
        kernel_spin_unlock(&space->lock);
        return 1;
    }
    level0[index0] = (uint64_t)page->physical_address | KERNEL_PTE_VALID |
                     KERNEL_PTE_USER | flags;
    *physical_address = page->physical_address;
    kernel_spin_unlock(&space->lock);
    return 0;
}

int kernel_address_space_map_physical(KernelAddressSpace *space,
                                      uintptr_t virtual_address,
                                      uintptr_t physical_address,
                                      size_t size,
                                      uint64_t flags)
{
    if (space == NULL || size == 0 ||
        ((uint64_t)virtual_address & KERNEL_PAGE_MASK) != 0 ||
        ((uint64_t)physical_address & KERNEL_PAGE_MASK) != 0 ||
        ((uint64_t)size & KERNEL_PAGE_MASK) != 0 ||
        (uint64_t)virtual_address < KERNEL_USER_IMAGE_BASE ||
        (uint64_t)virtual_address > UINT64_MAX - (uint64_t)size ||
        (uint64_t)virtual_address + (uint64_t)size > KERNEL_USER_STACK_TOP ||
        (uint64_t)physical_address > UINT64_MAX - (uint64_t)size ||
        kernel_phys_to_virt(physical_address) == NULL ||
        kernel_phys_to_virt(physical_address + size - 1) == NULL ||
        (flags & ~(KERNEL_PTE_READ | KERNEL_PTE_WRITE |
                   KERNEL_PTE_EXECUTE)) != 0 ||
        (flags & KERNEL_PTE_READ) == 0 ||
        ((flags & KERNEL_PTE_WRITE) != 0 &&
         (flags & KERNEL_PTE_EXECUTE) != 0)) {
        return 1;
    }

    kernel_spin_lock(&space->lock);
    uintptr_t mapped_end = virtual_address;
    for (size_t offset = 0; offset < size;
         offset += (size_t)KERNEL_PAGE_SIZE) {
        uintptr_t page = virtual_address + offset;
        size_t index2 = (size_t)(((uint64_t)page >> 30) & 0x1FF);
        size_t index1 = (size_t)(((uint64_t)page >> 21) & 0x1FF);
        size_t index0 = (size_t)(((uint64_t)page >> 12) & 0x1FF);
        if (index2 != 0) break;
        uint64_t *level0 = address_private_level0(space, index1);
        if (level0 == NULL || (level0[index0] & KERNEL_PTE_VALID) != 0) break;
        KernelAddressPage *record = kernel_malloc(sizeof(*record));
        if (record == NULL) break;
        record->node.previous = NULL;
        record->node.next = NULL;
        record->physical_address = physical_address + offset;
        record->virtual_address = page;
        record->kind = ADDRESS_PAGE_EXTERNAL;
        kernel_list_push_back(&space->pages, &record->node);
        level0[index0] = (uint64_t)(physical_address + offset) |
                         KERNEL_PTE_VALID | KERNEL_PTE_USER | flags;
        mapped_end = page + (uintptr_t)KERNEL_PAGE_SIZE;
    }
    if ((size_t)(mapped_end - virtual_address) == size) {
        kernel_spin_unlock(&space->lock);
        return 0;
    }
    for (uintptr_t page = virtual_address; page < mapped_end;
         page += (uintptr_t)KERNEL_PAGE_SIZE) {
        uint64_t *level0 = address_lookup_level0(space, page);
        size_t index0 = (size_t)(((uint64_t)page >> 12) & 0x1FF);
        KernelAddressPage *record = address_find_user_page(space, page);
        if (level0 != NULL) level0[index0] = 0;
        if (record != NULL) {
            kernel_list_remove(&space->pages, &record->node);
            kernel_free(record);
        }
    }
    kernel_spin_unlock(&space->lock);
    return 1;
}

static KernelAddressPage *address_find_user_page(
    KernelAddressSpace *space, uintptr_t virtual_address)
{
    KernelListNode *node = space->pages.sentinel.next;
    while (node != &space->pages.sentinel) {
        KernelAddressPage *page = (KernelAddressPage *)node;
        if ((page->kind == ADDRESS_PAGE_USER ||
             page->kind == ADDRESS_PAGE_EXTERNAL) &&
            page->virtual_address == virtual_address) {
            return page;
        }
        node = node->next;
    }
    return NULL;
}

static uint64_t *address_lookup_level0(KernelAddressSpace *space,
                                       uintptr_t virtual_address)
{
    uint64_t *root = kernel_phys_to_virt(space->root);
    if (root == NULL) return NULL;
    size_t index2 = (size_t)(((uint64_t)virtual_address >> 30) & 0x1FF);
    uint64_t entry = root[index2];
    if ((entry & KERNEL_PTE_VALID) == 0 ||
        (entry & KERNEL_PTE_PERMISSION_MASK) != 0) {
        return NULL;
    }
    uint64_t *level1 = kernel_phys_to_virt(
        (uintptr_t)(entry & KERNEL_PTE_ADDRESS_MASK));
    if (level1 == NULL) return NULL;
    size_t index1 = (size_t)(((uint64_t)virtual_address >> 21) & 0x1FF);
    entry = level1[index1];
    if ((entry & KERNEL_PTE_VALID) == 0 ||
        (entry & KERNEL_PTE_PERMISSION_MASK) != 0) {
        return NULL;
    }
    return kernel_phys_to_virt(
        (uintptr_t)(entry & KERNEL_PTE_ADDRESS_MASK));
}

int kernel_address_space_unmap_range(KernelAddressSpace *space,
                                     uintptr_t virtual_address,
                                     size_t size)
{
    if (space == NULL || size == 0 ||
        ((uint64_t)virtual_address & KERNEL_PAGE_MASK) != 0 ||
        ((uint64_t)size & KERNEL_PAGE_MASK) != 0 ||
        (uint64_t)virtual_address < KERNEL_USER_IMAGE_BASE ||
        (uint64_t)virtual_address > UINT64_MAX - (uint64_t)size ||
        (uint64_t)virtual_address + (uint64_t)size > KERNEL_USER_STACK_TOP) {
        return 1;
    }

    kernel_spin_lock(&space->lock);
    uintptr_t end = virtual_address + size;
    for (uintptr_t page = virtual_address; page < end;
         page += (uintptr_t)KERNEL_PAGE_SIZE) {
        uint64_t *level0 = address_lookup_level0(space, page);
        size_t index0 = (size_t)(((uint64_t)page >> 12) & 0x1FF);
        if (level0 == NULL || (level0[index0] & KERNEL_PTE_VALID) == 0 ||
            (level0[index0] & KERNEL_PTE_USER) == 0 ||
            address_find_user_page(space, page) == NULL) {
            kernel_spin_unlock(&space->lock);
            return 1;
        }
    }
    for (uintptr_t page = virtual_address; page < end;
         page += (uintptr_t)KERNEL_PAGE_SIZE) {
        uint64_t *level0 = address_lookup_level0(space, page);
        size_t index0 = (size_t)(((uint64_t)page >> 12) & 0x1FF);
        KernelAddressPage *record = address_find_user_page(space, page);
        level0[index0] = 0;
        kernel_list_remove(&space->pages, &record->node);
        if (record->kind != ADDRESS_PAGE_EXTERNAL) {
            kernel_pmm_free_page(record->physical_address);
        }
        kernel_free(record);
    }
    kernel_spin_unlock(&space->lock);
    return 0;
}

int kernel_address_space_resolve(const KernelAddressSpace *space,
                                 uintptr_t virtual_address,
                                 uint64_t required_flags,
                                 uintptr_t *physical_address)
{
    if (space == NULL || physical_address == NULL ||
        (required_flags & ~(KERNEL_PTE_READ | KERNEL_PTE_WRITE |
                            KERNEL_PTE_EXECUTE)) != 0 ||
        (uint64_t)virtual_address < KERNEL_USER_IMAGE_BASE ||
        (uint64_t)virtual_address >= KERNEL_USER_STACK_TOP) {
        return 0;
    }
    const uint64_t *root = kernel_phys_to_virt(space->root);
    if (root == NULL) return 0;
    size_t index2 = (size_t)(((uint64_t)virtual_address >> 30) & 0x1FF);
    uint64_t entry = root[index2];
    if ((entry & KERNEL_PTE_VALID) == 0) return 0;
    const uint64_t *level1 = kernel_phys_to_virt(
        (uintptr_t)(entry & KERNEL_PTE_ADDRESS_MASK));
    if (level1 == NULL) return 0;
    size_t index1 = (size_t)(((uint64_t)virtual_address >> 21) & 0x1FF);
    entry = level1[index1];
    if ((entry & KERNEL_PTE_VALID) == 0) return 0;
    const uint64_t *level0 = kernel_phys_to_virt(
        (uintptr_t)(entry & KERNEL_PTE_ADDRESS_MASK));
    if (level0 == NULL) return 0;
    size_t index0 = (size_t)(((uint64_t)virtual_address >> 12) & 0x1FF);
    entry = level0[index0];
    uint64_t required = KERNEL_PTE_VALID | KERNEL_PTE_USER |
                        required_flags;
    if ((entry & required) != required) return 0;
    *physical_address = (uintptr_t)(entry & KERNEL_PTE_ADDRESS_MASK) |
                        (virtual_address & (uintptr_t)KERNEL_PAGE_MASK);
    return 1;
}
