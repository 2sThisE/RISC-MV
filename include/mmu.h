#ifndef MMU_H
#define MMU_H

#include <stdint.h>

#include "ram.h"

#define MMU_PAGE_SHIFT 12U
#define MMU_PAGE_SIZE (UINT64_C(1) << MMU_PAGE_SHIFT)
#define MMU_PAGE_MASK (MMU_PAGE_SIZE - 1)
#define MMU_LARGE_PAGE_SHIFT 21U
#define MMU_LARGE_PAGE_SIZE (UINT64_C(1) << MMU_LARGE_PAGE_SHIFT)
#define MMU_LARGE_PAGE_MASK (MMU_LARGE_PAGE_SIZE - 1)
#define MMU_GIGA_PAGE_SHIFT 30U
#define MMU_GIGA_PAGE_SIZE (UINT64_C(1) << MMU_GIGA_PAGE_SHIFT)
#define MMU_GIGA_PAGE_MASK (MMU_GIGA_PAGE_SIZE - 1)
#define MMU_TABLE_ENTRY_SIZE 8U
#define MMU_TABLE_ENTRY_COUNT 512U
#define MMU_VIRTUAL_ADDRESS_BITS 39U
#define MMU_VIRTUAL_ADDRESS_LIMIT \
    (UINT64_C(1) << MMU_VIRTUAL_ADDRESS_BITS)

enum {
    MMU_PTE_VALID   = UINT64_C(1) << 0,
    MMU_PTE_READ    = UINT64_C(1) << 1,
    MMU_PTE_WRITE   = UINT64_C(1) << 2,
    MMU_PTE_EXECUTE = UINT64_C(1) << 3,
    MMU_PTE_USER    = UINT64_C(1) << 4
};

#define MMU_PTE_PERMISSION_MASK \
    (MMU_PTE_READ | MMU_PTE_WRITE | MMU_PTE_EXECUTE)
#define MMU_PTE_ADDRESS_MASK (~MMU_PAGE_MASK)
#define MMU_PTE_RESERVED_LOW_MASK \
    (MMU_PAGE_MASK & ~(MMU_PTE_VALID | MMU_PTE_PERMISSION_MASK | \
                       MMU_PTE_USER))

typedef enum {
    MMU_ACCESS_READ,
    MMU_ACCESS_WRITE,
    MMU_ACCESS_EXECUTE,
    MMU_ACCESS_ATOMIC
} MmuAccess;

typedef enum {
    MMU_RESULT_OK = 0,
    MMU_RESULT_INVALID_VIRTUAL_ADDRESS,
    MMU_RESULT_INVALID_ROOT,
    MMU_RESULT_NOT_PRESENT,
    MMU_RESULT_MALFORMED_ENTRY,
    MMU_RESULT_PERMISSION_DENIED
} MmuResult;

int mmu_root_valid(const RAM *ram, uint64_t root_address);
MmuResult mmu_translate(RAM *ram,
                        uint64_t root_address,
                        int user_mode,
                        uint64_t virtual_address,
                        MmuAccess access,
                        uint64_t *physical_address);

#endif
