#include "ram.h"

#include <stdint.h>

static int ram_range_valid(const RAM *ram,
                           uint64_t address,
                           size_t width)
{
    if (ram == NULL || ram->data == NULL || width == 0 || width > 8 ||
        address > SIZE_MAX) {
        return 0;
    }

    size_t start = (size_t)address;
    return start <= ram->size && width <= ram->size - start;
}

static size_t lock_index(uint64_t address)
{
    return (size_t)((address / RAM_LOCK_GRANULARITY) %
                    RAM_LOCK_STRIPE_COUNT);
}

static void lock_one(RAM *ram, size_t index)
{
    while (atomic_flag_test_and_set_explicit(&ram->locks[index],
                                             memory_order_acquire)) {
        /* RAM 임계 구역은 최대 8바이트 복사뿐이므로 짧게 spin한다. */
    }
}

static void unlock_one(RAM *ram, size_t index)
{
    atomic_flag_clear_explicit(&ram->locks[index], memory_order_release);
}

static void lock_range(RAM *ram,
                       uint64_t address,
                       size_t width,
                       size_t *first,
                       size_t *second)
{
    size_t start_lock = lock_index(address);
    size_t end_lock = lock_index(address + width - 1);

    *first = start_lock < end_lock ? start_lock : end_lock;
    *second = start_lock < end_lock ? end_lock : start_lock;

    lock_one(ram, *first);
    if (*second != *first) {
        lock_one(ram, *second);
    }
}

static void unlock_range(RAM *ram, size_t first, size_t second)
{
    if (second != first) {
        unlock_one(ram, second);
    }
    unlock_one(ram, first);
}

int ram_enable_synchronization(RAM *ram)
{
    if (ram == NULL || ram->data == NULL) {
        return 0;
    }

    if (!ram->synchronization_ready) {
        for (size_t i = 0; i < RAM_LOCK_STRIPE_COUNT; ++i) {
            atomic_flag_clear(&ram->locks[i]);
        }
        ram->synchronization_ready = 1;
    }

    return 1;
}

static int naturally_aligned(uint64_t address, size_t width)
{
    return (address & (uint64_t)(width - 1)) == 0;
}

int ram_read(RAM *ram,
             uint64_t address,
             size_t width,
             uint64_t *value)
{
    if (value == NULL || !ram_range_valid(ram, address, width) ||
        !ram_enable_synchronization(ram)) {
        return 0;
    }

    uint8_t *source = ram->data + (size_t)address;

#if defined(__GNUC__) || defined(__clang__)
    if (naturally_aligned(address, width)) {
        switch (width) {
            case 1:
                *value = __atomic_load_n((uint8_t *)source,
                                         __ATOMIC_SEQ_CST);
                return 1;
            case 2:
                *value = __atomic_load_n((uint16_t *)source,
                                         __ATOMIC_SEQ_CST);
                return 1;
            case 4:
                *value = __atomic_load_n((uint32_t *)source,
                                         __ATOMIC_SEQ_CST);
                return 1;
            case 8:
                *value = __atomic_load_n((uint64_t *)source,
                                         __ATOMIC_SEQ_CST);
                return 1;
        }
    }
#endif

    size_t first;
    size_t second;
    lock_range(ram, address, width, &first, &second);

    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i) {
#if defined(__GNUC__) || defined(__clang__)
        uint8_t byte = __atomic_load_n(&source[i], __ATOMIC_SEQ_CST);
#else
        uint8_t byte = source[i];
#endif
        result |= (uint64_t)byte << (i * 8);
    }

    unlock_range(ram, first, second);
    *value = result;
    return 1;
}

int ram_write(RAM *ram,
              uint64_t address,
              size_t width,
              uint64_t value)
{
    if (!ram_range_valid(ram, address, width) ||
        !ram_enable_synchronization(ram)) {
        return 0;
    }

    uint8_t *destination = ram->data + (size_t)address;

#if defined(__GNUC__) || defined(__clang__)
    if (naturally_aligned(address, width)) {
        switch (width) {
            case 1:
                __atomic_store_n((uint8_t *)destination,
                                 (uint8_t)value,
                                 __ATOMIC_SEQ_CST);
                return 1;
            case 2:
                __atomic_store_n((uint16_t *)destination,
                                 (uint16_t)value,
                                 __ATOMIC_SEQ_CST);
                return 1;
            case 4:
                __atomic_store_n((uint32_t *)destination,
                                 (uint32_t)value,
                                 __ATOMIC_SEQ_CST);
                return 1;
            case 8:
                __atomic_store_n((uint64_t *)destination,
                                 value,
                                 __ATOMIC_SEQ_CST);
                return 1;
        }
    }
#endif

    size_t first;
    size_t second;
    lock_range(ram, address, width, &first, &second);

    for (size_t i = 0; i < width; ++i) {
        uint8_t byte = (uint8_t)(value >> (i * 8));
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&destination[i], byte, __ATOMIC_SEQ_CST);
#else
        destination[i] = byte;
#endif
    }

    unlock_range(ram, first, second);
    return 1;
}

static int ram_block_range_valid(const RAM *ram,
                                 uint64_t address,
                                 size_t size)
{
    if (ram == NULL || ram->data == NULL || address > SIZE_MAX) {
        return 0;
    }
    size_t start = (size_t)address;
    return start <= ram->size && size <= ram->size - start;
}

static size_t block_chunk_width(uint64_t address, size_t remaining)
{
    if (remaining >= 8 && (address & UINT64_C(7)) == 0) {
        return 8;
    }
    if (remaining >= 4 && (address & UINT64_C(3)) == 0) {
        return 4;
    }
    if (remaining >= 2 && (address & UINT64_C(1)) == 0) {
        return 2;
    }
    return 1;
}

int ram_read_block(RAM *ram,
                   uint64_t address,
                   void *buffer,
                   size_t size)
{
    if ((buffer == NULL && size != 0) ||
        !ram_block_range_valid(ram, address, size) ||
        !ram_enable_synchronization(ram)) {
        return 0;
    }

    uint8_t *destination = buffer;
    size_t copied = 0;
    while (copied < size) {
        uint64_t cursor = address + copied;
        size_t width = block_chunk_width(cursor, size - copied);
        uint64_t value;
        if (!ram_read(ram, cursor, width, &value)) {
            return 0;
        }
        for (size_t i = 0; i < width; ++i) {
            destination[copied + i] = (uint8_t)(value >> (i * 8));
        }
        copied += width;
    }
    return 1;
}

int ram_write_block(RAM *ram,
                    uint64_t address,
                    const void *buffer,
                    size_t size)
{
    if ((buffer == NULL && size != 0) ||
        !ram_block_range_valid(ram, address, size) ||
        !ram_enable_synchronization(ram)) {
        return 0;
    }

    const uint8_t *source = buffer;
    size_t copied = 0;
    while (copied < size) {
        uint64_t cursor = address + copied;
        size_t width = block_chunk_width(cursor, size - copied);
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) {
            value |= (uint64_t)source[copied + i] << (i * 8);
        }
        if (!ram_write(ram, cursor, width, value)) {
            return 0;
        }
        copied += width;
    }
    return 1;
}

static int atomic64_address_valid(RAM *ram, uint64_t address)
{
    return naturally_aligned(address, 8) &&
           ram_range_valid(ram, address, 8) &&
           ram_enable_synchronization(ram);
}

int ram_compare_exchange64(RAM *ram,
                           uint64_t address,
                           uint64_t *expected,
                           uint64_t desired)
{
    if (expected == NULL || !atomic64_address_valid(ram, address)) {
        return 0;
    }

#if defined(__GNUC__) || defined(__clang__)
    (void)__atomic_compare_exchange_n(
        (uint64_t *)(ram->data + (size_t)address),
        expected,
        desired,
        0,
        __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
    return 1;
#else
    size_t stripe = lock_index(address);
    lock_one(ram, stripe);
    uint64_t current = 0;
    for (size_t i = 0; i < 8; ++i) {
        current |= (uint64_t)ram->data[(size_t)address + i] << (i * 8);
    }
    if (current == *expected) {
        for (size_t i = 0; i < 8; ++i) {
            ram->data[(size_t)address + i] =
                (uint8_t)(desired >> (i * 8));
        }
    } else {
        *expected = current;
    }
    unlock_one(ram, stripe);
    return 1;
#endif
}

int ram_exchange64(RAM *ram, uint64_t address, uint64_t *value)
{
    if (value == NULL || !atomic64_address_valid(ram, address)) {
        return 0;
    }

#if defined(__GNUC__) || defined(__clang__)
    *value = __atomic_exchange_n(
        (uint64_t *)(ram->data + (size_t)address),
        *value,
        __ATOMIC_SEQ_CST);
    return 1;
#else
    size_t stripe = lock_index(address);
    lock_one(ram, stripe);
    uint64_t current = 0;
    for (size_t i = 0; i < 8; ++i) {
        current |= (uint64_t)ram->data[(size_t)address + i] << (i * 8);
    }
    for (size_t i = 0; i < 8; ++i) {
        ram->data[(size_t)address + i] =
            (uint8_t)(*value >> (i * 8));
    }
    *value = current;
    unlock_one(ram, stripe);
    return 1;
#endif
}

int ram_fetch_add64(RAM *ram, uint64_t address, uint64_t *value)
{
    if (value == NULL || !atomic64_address_valid(ram, address)) {
        return 0;
    }

#if defined(__GNUC__) || defined(__clang__)
    *value = __atomic_fetch_add(
        (uint64_t *)(ram->data + (size_t)address),
        *value,
        __ATOMIC_SEQ_CST);
    return 1;
#else
    size_t stripe = lock_index(address);
    lock_one(ram, stripe);
    uint64_t current = 0;
    for (size_t i = 0; i < 8; ++i) {
        current |= (uint64_t)ram->data[(size_t)address + i] << (i * 8);
    }
    uint64_t desired = current + *value;
    for (size_t i = 0; i < 8; ++i) {
        ram->data[(size_t)address + i] =
            (uint8_t)(desired >> (i * 8));
    }
    *value = current;
    unlock_one(ram, stripe);
    return 1;
#endif
}

void ram_memory_fence(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

int parse_ram_size(const char *text, size_t *result)
{
    if (text == NULL || result == NULL || *text == '\0') {
        return 0;
    }

    const char *cursor = text;
    size_t value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        size_t digit = (size_t)(*cursor - '0');
        if (value > (SIZE_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
        ++cursor;
    }
    if (cursor == text || value == 0) return 0;

    size_t multiplier = 1;
    if (*cursor != '\0') {
        char unit = *cursor++;
        if (*cursor != '\0') return 0;
        if (unit >= 'A' && unit <= 'Z') {
            unit = (char)(unit - 'A' + 'a');
        }
        if (unit == 'b') multiplier = 1;
        else if (unit == 'k') multiplier = (size_t)1024;
        else if (unit == 'm') multiplier = (size_t)1024 * 1024;
        else if (unit == 'g') {
            multiplier = (size_t)1024 * 1024 * 1024;
        } else {
            return 0;
        }
    }

    if (value > SIZE_MAX / multiplier) return 0;

    *result = value * multiplier;
    return 1;
}
