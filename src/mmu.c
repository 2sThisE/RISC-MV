#include "mmu.h"

#include <stddef.h>

static int physical_range_valid(const RAM *ram,
                                uint64_t address,
                                size_t width)
{
    if (ram == NULL || ram->data == NULL || address > SIZE_MAX) {
        return 0;
    }

    size_t start = (size_t)address;
    return start <= ram->size && width <= ram->size - start;
}

int mmu_root_valid(const RAM *ram, uint64_t root_address)
{
    return (root_address & MMU_PAGE_MASK) == 0 &&
           physical_range_valid(ram, root_address, (size_t)MMU_PAGE_SIZE);
}

static int permission_granted(uint64_t pte,
                              int user_mode,
                              MmuAccess access)
{
    if (user_mode && (pte & MMU_PTE_USER) == 0) {
        return 0;
    }

    switch (access) {
        case MMU_ACCESS_READ:
            return (pte & MMU_PTE_READ) != 0;
        case MMU_ACCESS_WRITE:
            return (pte & MMU_PTE_WRITE) != 0;
        case MMU_ACCESS_EXECUTE:
            return (pte & MMU_PTE_EXECUTE) != 0;
        case MMU_ACCESS_ATOMIC:
            return (pte & (MMU_PTE_READ | MMU_PTE_WRITE)) ==
                   (MMU_PTE_READ | MMU_PTE_WRITE);
    }

    return 0;
}

MmuResult mmu_translate(RAM *ram,
                        uint64_t root_address,
                        int user_mode,
                        uint64_t virtual_address,
                        MmuAccess access,
                        uint64_t *physical_address)
{
    if (physical_address == NULL ||
        virtual_address >= MMU_VIRTUAL_ADDRESS_LIMIT) {
        return MMU_RESULT_INVALID_VIRTUAL_ADDRESS;
    }
    if (!mmu_root_valid(ram, root_address)) {
        return MMU_RESULT_INVALID_ROOT;
    }

    const unsigned int shifts[3] = {30U, 21U, 12U};
    uint64_t table_address = root_address;

    for (size_t level = 0; level < 3; ++level) {
        uint64_t index = (virtual_address >> shifts[level]) & UINT64_C(0x1FF);
        uint64_t entry_address = table_address +
                                 index * MMU_TABLE_ENTRY_SIZE;
        uint64_t pte;

        if (!ram_read(ram, entry_address, MMU_TABLE_ENTRY_SIZE, &pte)) {
            return MMU_RESULT_INVALID_ROOT;
        }
        if ((pte & MMU_PTE_VALID) == 0) {
            return MMU_RESULT_NOT_PRESENT;
        }
        if ((pte & MMU_PTE_RESERVED_LOW_MASK) != 0 ||
            ((pte & MMU_PTE_WRITE) != 0 &&
             (pte & MMU_PTE_READ) == 0)) {
            return MMU_RESULT_MALFORMED_ENTRY;
        }

        uint64_t permissions = pte & MMU_PTE_PERMISSION_MASK;
        uint64_t next_address = pte & MMU_PTE_ADDRESS_MASK;

        if (level < 2) {
            if (permissions != 0 || (pte & MMU_PTE_USER) != 0 ||
                !mmu_root_valid(ram, next_address)) {
                return MMU_RESULT_MALFORMED_ENTRY;
            }
            table_address = next_address;
            continue;
        }

        if (permissions == 0) {
            return MMU_RESULT_MALFORMED_ENTRY;
        }
        if (!permission_granted(pte, user_mode, access)) {
            return MMU_RESULT_PERMISSION_DENIED;
        }
        if (next_address < (uint64_t)ram->size &&
            !physical_range_valid(ram,
                                  next_address,
                                  (size_t)MMU_PAGE_SIZE)) {
            return MMU_RESULT_MALFORMED_ENTRY;
        }

        *physical_address = next_address |
                            (virtual_address & MMU_PAGE_MASK);
        return MMU_RESULT_OK;
    }

    return MMU_RESULT_MALFORMED_ENTRY;
}
