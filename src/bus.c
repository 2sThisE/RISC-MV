#include "bus.h"

static void mapping_lock(BusMapping *mapping)
{
    while (atomic_flag_test_and_set_explicit(&mapping->lock,
                                             memory_order_acquire)) {
    }
}

static void mapping_unlock(BusMapping *mapping)
{
    atomic_flag_clear_explicit(&mapping->lock, memory_order_release);
}

static void mapping_table_lock(Bus *bus)
{
    while (atomic_flag_test_and_set_explicit(&bus->mapping_table_lock,
                                             memory_order_acquire)) {
    }
}

static void mapping_table_unlock(Bus *bus)
{
    atomic_flag_clear_explicit(&bus->mapping_table_lock,
                               memory_order_release);
}

static int range_valid(uint64_t base, uint64_t size)
{
    return size != 0 && base <= UINT64_MAX - (size - 1);
}

static int ranges_overlap(uint64_t first_base,
                          uint64_t first_size,
                          uint64_t second_base,
                          uint64_t second_size)
{
    uint64_t first_end = first_base + first_size - 1;
    uint64_t second_end = second_base + second_size - 1;
    return first_base <= second_end && second_base <= first_end;
}

static int mapping_contains(const BusMapping *mapping,
                            uint64_t address,
                            size_t width)
{
    if (address < mapping->base) {
        return 0;
    }

    uint64_t offset = address - mapping->base;
    return offset < mapping->size &&
           (uint64_t)width <= mapping->size - offset;
}

static int rom_contains(const BusRomMapping *rom,
                        uint64_t address,
                        size_t width)
{
    if (!rom->present || address < rom->base) {
        return 0;
    }
    uint64_t offset = address - rom->base;
    return offset < rom->size &&
           (uint64_t)width <= rom->size - offset;
}

static int rom_read(const BusRomMapping *rom,
                    uint64_t address,
                    size_t width,
                    uint64_t *value)
{
    if (value == NULL || width == 0 || width > sizeof(uint64_t) ||
        !rom_contains(rom, address, width)) {
        return 0;
    }
    size_t offset = (size_t)(address - rom->base);
    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i) {
        result |= (uint64_t)rom->data[offset + i] << (i * 8);
    }
    *value = result;
    return 1;
}

static BusMapping *lock_mapping_for_address(Bus *bus,
                                            uint64_t address,
                                            size_t width)
{
    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        BusMapping *mapping = &bus->mappings[i];
        if (!atomic_load_explicit(&mapping->present,
                                  memory_order_acquire)) {
            continue;
        }
        mapping_lock(mapping);
        if (atomic_load_explicit(&mapping->present,
                                 memory_order_relaxed) &&
            mapping_contains(mapping, address, width)) {
            return mapping;
        }
        mapping_unlock(mapping);
    }

    return NULL;
}

static int ram_contains(const RAM *ram, uint64_t address, size_t width)
{
    if (address > SIZE_MAX) {
        return 0;
    }

    size_t start = (size_t)address;
    return start <= ram->size && width <= ram->size - start;
}

int bus_init(Bus *bus, RAM *ram)
{
    if (bus == NULL || ram == NULL || ram->data == NULL) {
        return 0;
    }

    if (!ram_enable_synchronization(ram)) {
        return 0;
    }

    bus->ram = ram;
    bus->rom = (BusRomMapping){0};
    bus->mapping_count = 0;
    atomic_flag_clear(&bus->mapping_table_lock);
    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        BusMapping *mapping = &bus->mappings[i];
        mapping->base = 0;
        mapping->size = 0;
        mapping->device = (BusDevice){0};
        atomic_flag_clear(&mapping->lock);
        atomic_init(&mapping->present, 0);
    }
    return 1;
}

int bus_map_device(Bus *bus,
                   uint64_t base,
                   uint64_t size,
                   BusDevice device)
{
    if (bus == NULL || bus->ram == NULL ||
        !range_valid(base, size) ||
        (device.read == NULL && device.write == NULL &&
         device.tick == NULL && device.reset == NULL)) {
        return 0;
    }

    if (bus->ram->size != 0 &&
        ranges_overlap(base, size, 0, (uint64_t)bus->ram->size)) {
        return 0;
    }

    mapping_table_lock(bus);
    if (bus->mapping_count >= BUS_MAX_DEVICES) {
        mapping_table_unlock(bus);
        return 0;
    }

    if (bus->rom.present &&
        ranges_overlap(base, size, bus->rom.base, bus->rom.size)) {
        mapping_table_unlock(bus);
        return 0;
    }

    size_t free_index = BUS_MAX_DEVICES;
    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        const BusMapping *mapping = &bus->mappings[i];
        if (!atomic_load_explicit(&mapping->present,
                                  memory_order_acquire)) {
            if (free_index == BUS_MAX_DEVICES) {
                free_index = i;
            }
            continue;
        }
        if (ranges_overlap(base, size, mapping->base, mapping->size)) {
            mapping_table_unlock(bus);
            return 0;
        }
    }

    if (free_index == BUS_MAX_DEVICES) {
        mapping_table_unlock(bus);
        return 0;
    }

    BusMapping *mapping = &bus->mappings[free_index];
    mapping_lock(mapping);
    mapping->base = base;
    mapping->size = size;
    mapping->device = device;
    atomic_store_explicit(&mapping->present, 1, memory_order_release);
    ++bus->mapping_count;
    mapping_unlock(mapping);
    mapping_table_unlock(bus);
    return 1;
}

int bus_map_rom(Bus *bus,
                uint64_t base,
                const uint8_t *data,
                size_t size)
{
    if (bus == NULL || bus->ram == NULL || data == NULL || size == 0 ||
        !range_valid(base, (uint64_t)size) ||
        (bus->ram->size != 0 &&
         ranges_overlap(base,
                        (uint64_t)size,
                        0,
                        (uint64_t)bus->ram->size))) {
        return 0;
    }

    mapping_table_lock(bus);
    if (bus->rom.present) {
        mapping_table_unlock(bus);
        return 0;
    }
    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        const BusMapping *mapping = &bus->mappings[i];
        if (atomic_load_explicit(&mapping->present,
                                 memory_order_acquire) &&
            ranges_overlap(base,
                           (uint64_t)size,
                           mapping->base,
                           mapping->size)) {
            mapping_table_unlock(bus);
            return 0;
        }
    }
    bus->rom = (BusRomMapping){
        .base = base,
        .size = (uint64_t)size,
        .data = data,
        .present = 1
    };
    mapping_table_unlock(bus);
    return 1;
}

int bus_fetch(Bus *bus,
              uint64_t address,
              size_t width,
              uint64_t *value)
{
    if (bus == NULL || bus->ram == NULL || value == NULL ||
        width == 0 || width > sizeof(uint64_t)) {
        return 0;
    }
    if (ram_contains(bus->ram, address, width)) {
        return ram_read(bus->ram, address, width, value);
    }
    return rom_read(&bus->rom, address, width, value);
}

int bus_unmap_device(Bus *bus,
                     uint64_t base,
                     uint64_t size)
{
    if (bus == NULL || bus->ram == NULL || !range_valid(base, size)) {
        return 0;
    }

    mapping_table_lock(bus);
    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        BusMapping *mapping = &bus->mappings[i];
        if (!atomic_load_explicit(&mapping->present,
                                  memory_order_acquire) ||
            mapping->base != base || mapping->size != size) {
            continue;
        }

        mapping_lock(mapping);
        atomic_store_explicit(&mapping->present, 0, memory_order_release);
        mapping->base = 0;
        mapping->size = 0;
        mapping->device = (BusDevice){0};
        --bus->mapping_count;
        mapping_unlock(mapping);
        mapping_table_unlock(bus);
        return 1;
    }

    mapping_table_unlock(bus);
    return 0;
}

int bus_read(Bus *bus,
             uint64_t address,
             size_t width,
             uint64_t *value)
{
    if (bus == NULL || bus->ram == NULL || value == NULL ||
        width == 0 || width > sizeof(uint64_t)) {
        return 0;
    }

    BusMapping *mapping = lock_mapping_for_address(bus, address, width);
    if (mapping != NULL) {
        if (mapping->device.read == NULL) {
            mapping_unlock(mapping);
            return 0;
        }
        int result = mapping->device.read(mapping->device.context,
                                          address - mapping->base,
                                          width,
                                          value);
        mapping_unlock(mapping);
        return result;
    }


    if (rom_read(&bus->rom, address, width, value)) {
        return 1;
    }

    if (!ram_contains(bus->ram, address, width)) {
        return 0;
    }

    return ram_read(bus->ram, address, width, value);
}

int bus_write(Bus *bus,
              uint64_t address,
              size_t width,
              uint64_t value)
{
    if (bus == NULL || bus->ram == NULL ||
        width == 0 || width > sizeof(uint64_t)) {
        return 0;
    }

    BusMapping *mapping = lock_mapping_for_address(bus, address, width);
    if (mapping != NULL) {
        if (mapping->device.write == NULL) {
            mapping_unlock(mapping);
            return 0;
        }
        int result = mapping->device.write(mapping->device.context,
                                           address - mapping->base,
                                           width,
                                           value);
        mapping_unlock(mapping);
        return result;
    }


    if (rom_contains(&bus->rom, address, width)) {
        return 0;
    }

    if (!ram_contains(bus->ram, address, width)) {
        return 0;
    }

    return ram_write(bus->ram, address, width, value);
}

int bus_compare_exchange64(Bus *bus,
                           uint64_t address,
                           uint64_t *expected,
                           uint64_t desired)
{
    if (bus == NULL || bus->ram == NULL || expected == NULL ||
        !ram_contains(bus->ram, address, 8)) {
        return 0;
    }

    return ram_compare_exchange64(bus->ram, address, expected, desired);
}

int bus_exchange64(Bus *bus, uint64_t address, uint64_t *value)
{
    if (bus == NULL || bus->ram == NULL || value == NULL ||
        !ram_contains(bus->ram, address, 8)) {
        return 0;
    }

    return ram_exchange64(bus->ram, address, value);
}

int bus_fetch_add64(Bus *bus, uint64_t address, uint64_t *value)
{
    if (bus == NULL || bus->ram == NULL || value == NULL ||
        !ram_contains(bus->ram, address, 8)) {
        return 0;
    }

    return ram_fetch_add64(bus->ram, address, value);
}

void bus_memory_fence(void)
{
    ram_memory_fence();
}

void bus_tick(Bus *bus,
              uint64_t ticks,
              InterruptRouter *interrupts)
{
    if (bus == NULL || ticks == 0) {
        return;
    }

    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        BusMapping *mapping = &bus->mappings[i];
        if (!atomic_load_explicit(&mapping->present,
                                  memory_order_acquire)) {
            continue;
        }
        mapping_lock(mapping);
        if (atomic_load_explicit(&mapping->present,
                                 memory_order_relaxed) &&
            mapping->device.tick != NULL) {
            mapping->device.tick(mapping->device.context,
                                 ticks,
                                 interrupts);
        }
        mapping_unlock(mapping);
    }
}

void bus_reset(Bus *bus)
{
    if (bus == NULL) {
        return;
    }

    for (size_t i = 0; i < BUS_MAX_DEVICES; ++i) {
        BusMapping *mapping = &bus->mappings[i];
        if (!atomic_load_explicit(&mapping->present,
                                  memory_order_acquire)) {
            continue;
        }
        mapping_lock(mapping);
        if (atomic_load_explicit(&mapping->present,
                                 memory_order_relaxed) &&
            mapping->device.reset != NULL) {
            mapping->device.reset(mapping->device.context);
        }
        mapping_unlock(mapping);
    }
}
