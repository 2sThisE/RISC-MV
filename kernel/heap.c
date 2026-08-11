#include "kernel_internal.h"
#include "kernel_runtime.h"

#define KERNEL_HEAP_ALIGNMENT ((size_t)16)
#define KERNEL_HEAP_GUARD_SIZE UINT64_C(0x200000)
#define KERNEL_HEAP_MAX_SIZE UINT64_C(0x04000000)
#define KERNEL_HEAP_MAGIC UINT64_C(0x434F4C424D56434B)
#define KERNEL_HEAP_FREE UINT64_C(1)

typedef struct KernelHeapBlock {
    uint64_t magic;
    size_t size;
    struct KernelHeapBlock *previous;
    struct KernelHeapBlock *next;
    uint64_t flags;
    uint64_t reserved;
} KernelHeapBlock;

static uintptr_t heap_base;
static uintptr_t heap_limit;
static size_t heap_mapped_size;
static KernelHeapBlock *heap_first;
static KernelHeapBlock *heap_last;
static KernelSpinLock heap_lock;

static int heap_align_size(size_t size, size_t *aligned)
{
    if (size == 0 || size > KERNEL_SIZE_MAX -
                              (KERNEL_HEAP_ALIGNMENT - 1)) {
        return 0;
    }
    *aligned = (size + (KERNEL_HEAP_ALIGNMENT - 1)) &
               ~(KERNEL_HEAP_ALIGNMENT - 1);
    return 1;
}

static void heap_zero(void *pointer, size_t size)
{
    uint8_t *bytes = pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static int heap_block_free(const KernelHeapBlock *block)
{
    return (block->flags & KERNEL_HEAP_FREE) != 0;
}

static int heap_append_pages(size_t minimum_payload)
{
    if (minimum_payload > KERNEL_SIZE_MAX - sizeof(KernelHeapBlock)) return 0;
    size_t needed = minimum_payload + sizeof(KernelHeapBlock);
    if (needed > KERNEL_SIZE_MAX - (size_t)KERNEL_PAGE_MASK) return 0;
    needed = (needed + (size_t)KERNEL_PAGE_MASK) &
             ~(size_t)KERNEL_PAGE_MASK;

    size_t old_size = heap_mapped_size;
    if (old_size >= (size_t)(heap_limit - heap_base) ||
        needed > (size_t)(heap_limit - heap_base) - old_size) {
        return 0;
    }

    size_t added = 0;
    while (added < needed) {
        uintptr_t physical = kernel_pmm_alloc_page();
        if (physical == 0) break;
        if (kernel_map_page(kernel_page_table_root(),
                            heap_base + old_size + added,
                            physical,
                            KERNEL_PTE_VALID | KERNEL_PTE_READ |
                                KERNEL_PTE_WRITE) != 0) {
            kernel_pmm_free_page(physical);
            break;
        }
        added += (size_t)KERNEL_PAGE_SIZE;
        heap_mapped_size = old_size + added;
    }
    if (added == 0) return 0;

    if (heap_last != NULL && heap_block_free(heap_last)) {
        heap_last->size += added;
    } else {
        KernelHeapBlock *block =
            (KernelHeapBlock *)(heap_base + old_size);
        block->magic = KERNEL_HEAP_MAGIC;
        block->size = added - sizeof(KernelHeapBlock);
        block->previous = heap_last;
        block->next = NULL;
        block->flags = KERNEL_HEAP_FREE;
        block->reserved = 0;
        if (heap_last != NULL) heap_last->next = block;
        else heap_first = block;
        heap_last = block;
    }
    return added >= needed;
}

static void heap_split(KernelHeapBlock *block, size_t size)
{
    if (block->size < size + sizeof(KernelHeapBlock) +
                          KERNEL_HEAP_ALIGNMENT) {
        return;
    }
    KernelHeapBlock *tail = (KernelHeapBlock *)
        ((uint8_t *)(block + 1) + size);
    tail->magic = KERNEL_HEAP_MAGIC;
    tail->size = block->size - size - sizeof(KernelHeapBlock);
    tail->previous = block;
    tail->next = block->next;
    tail->flags = KERNEL_HEAP_FREE;
    tail->reserved = 0;
    if (tail->next != NULL) tail->next->previous = tail;
    else heap_last = tail;
    block->next = tail;
    block->size = size;
}

static void heap_merge_next(KernelHeapBlock *block)
{
    KernelHeapBlock *next = block->next;
    if (next == NULL || !heap_block_free(next)) return;
    block->size += sizeof(KernelHeapBlock) + next->size;
    block->next = next->next;
    if (block->next != NULL) block->next->previous = block;
    else heap_last = block;
    next->magic = 0;
}

int kernel_heap_init(void)
{
    uint64_t direct_end = kernel_virtual_handoff.direct_map_base;
    if (direct_end > UINT64_MAX - kernel_virtual_handoff.direct_map_size) {
        return 1;
    }
    direct_end += kernel_virtual_handoff.direct_map_size;
    if (direct_end > UINT64_MAX - KERNEL_HEAP_GUARD_SIZE -
                                       (KERNEL_PAGE_SIZE - 1)) {
        return 1;
    }
    uint64_t selected = direct_end + KERNEL_HEAP_GUARD_SIZE;
    selected = (selected + KERNEL_PAGE_SIZE - 1) &
               KERNEL_PTE_ADDRESS_MASK;
    if (selected >= KERNEL_DEMAND_TEST_ADDRESS ||
        KERNEL_HEAP_MAX_SIZE > KERNEL_DEMAND_TEST_ADDRESS - selected) {
        return 1;
    }

    heap_base = (uintptr_t)selected;
    heap_limit = heap_base + (uintptr_t)KERNEL_HEAP_MAX_SIZE;
    heap_mapped_size = 0;
    heap_first = NULL;
    heap_last = NULL;
    kernel_spin_init(&heap_lock);
    return heap_append_pages((size_t)KERNEL_PAGE_SIZE) ? 0 : 1;
}

void *kernel_malloc(size_t size)
{
    size_t aligned;
    if (!heap_align_size(size, &aligned) || heap_first == NULL) return NULL;
    kernel_spin_lock(&heap_lock);
    for (;;) {
        for (KernelHeapBlock *block = heap_first;
             block != NULL;
             block = block->next) {
            if (block->magic == KERNEL_HEAP_MAGIC &&
                heap_block_free(block) && block->size >= aligned) {
                heap_split(block, aligned);
                block->flags &= ~KERNEL_HEAP_FREE;
                kernel_spin_unlock(&heap_lock);
                return block + 1;
            }
        }
        if (!heap_append_pages(aligned)) {
            kernel_spin_unlock(&heap_lock);
            return NULL;
        }
    }
}

void *kernel_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > KERNEL_SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *pointer = kernel_malloc(total);
    if (pointer != NULL) heap_zero(pointer, total);
    return pointer;
}

void kernel_free(void *pointer)
{
    if (pointer == NULL || heap_first == NULL) return;
    uintptr_t address = (uintptr_t)pointer;
    if (address < heap_base + sizeof(KernelHeapBlock) ||
        address >= heap_base + heap_mapped_size ||
        (address & (KERNEL_HEAP_ALIGNMENT - 1)) != 0) {
        return;
    }

    kernel_spin_lock(&heap_lock);
    KernelHeapBlock *block = ((KernelHeapBlock *)pointer) - 1;
    if (block->magic != KERNEL_HEAP_MAGIC || heap_block_free(block)) {
        kernel_spin_unlock(&heap_lock);
        return;
    }
    block->flags |= KERNEL_HEAP_FREE;
    heap_merge_next(block);
    if (block->previous != NULL && heap_block_free(block->previous)) {
        block = block->previous;
        heap_merge_next(block);
    }
    kernel_spin_unlock(&heap_lock);
}

int kernel_heap_validate(void)
{
    if (heap_first == NULL || heap_last == NULL || heap_mapped_size == 0) {
        return 1;
    }
    uintptr_t expected = heap_base;
    uintptr_t heap_end = heap_base + heap_mapped_size;
    KernelHeapBlock *previous = NULL;
    size_t blocks = 0;
    for (KernelHeapBlock *block = heap_first;
         block != NULL;
         block = block->next) {
        uintptr_t payload = (uintptr_t)(block + 1);
        if (++blocks > heap_mapped_size / sizeof(KernelHeapBlock) ||
            (uintptr_t)block != expected ||
            block->magic != KERNEL_HEAP_MAGIC ||
            block->previous != previous ||
            block->reserved != 0 ||
            (block->size & (KERNEL_HEAP_ALIGNMENT - 1)) != 0 ||
            payload > heap_end || block->size > heap_end - payload ||
            (block->next != NULL && heap_block_free(block) &&
             heap_block_free(block->next))) {
            return 1;
        }
        expected = payload + block->size;
        previous = block;
    }
    return previous != heap_last || expected != heap_end;
}

int kernel_heap_self_test(void)
{
    uint8_t *small = kernel_malloc(1);
    uint8_t *reusable = kernel_malloc(31);
    uint8_t *medium = kernel_malloc(1000);
    uint8_t *large = kernel_malloc(5000);
    if (small == NULL || reusable == NULL || medium == NULL || large == NULL ||
        ((uintptr_t)small & (KERNEL_HEAP_ALIGNMENT - 1)) != 0 ||
        ((uintptr_t)large & (KERNEL_HEAP_ALIGNMENT - 1)) != 0) {
        return 1;
    }
    small[0] = 0x11;
    for (size_t i = 0; i < 1000; ++i) medium[i] = (uint8_t)i;
    for (size_t i = 0; i < 5000; ++i) large[i] = (uint8_t)(i ^ 0xA5);
    for (size_t i = 0; i < 1000; ++i) {
        if (medium[i] != (uint8_t)i) return 1;
    }
    for (size_t i = 0; i < 5000; ++i) {
        if (large[i] != (uint8_t)(i ^ 0xA5)) return 1;
    }

    kernel_free(reusable);
    uint8_t *reused = kernel_malloc(24);
    if (reused != reusable) return 1;
    uint64_t *zeroed = kernel_calloc(17, sizeof(uint64_t));
    if (zeroed == NULL) return 1;
    for (size_t i = 0; i < 17; ++i) {
        if (zeroed[i] != 0) return 1;
    }
    if (kernel_calloc(KERNEL_SIZE_MAX, 2) != NULL) return 1;

    kernel_free(zeroed);
    kernel_free(reused);
    kernel_free(small);
    kernel_free(medium);
    kernel_free(large);
    if (kernel_heap_validate() != 0) return 1;

    void *coalesced = kernel_malloc(9000);
    if (coalesced == NULL) return 1;
    kernel_free(coalesced);
    return kernel_heap_validate();
}
