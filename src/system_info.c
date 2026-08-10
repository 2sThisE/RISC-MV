#include "system_info.h"

#include <stddef.h>

static int system_info_read(void *context,
                            uint64_t offset,
                            size_t width,
                            uint64_t *value)
{
    const SystemInfoDevice *device = context;
    if (device == NULL || value == NULL || width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case SYSTEM_INFO_MAGIC_OFFSET:
            *value = SYSTEM_INFO_MAGIC;
            return 1;
        case SYSTEM_INFO_VERSION_OFFSET:
            *value = SYSTEM_INFO_VERSION;
            return 1;
        case SYSTEM_INFO_FEATURES_OFFSET:
            *value = device->features;
            return 1;
        case SYSTEM_INFO_RAM_BASE_OFFSET:
            *value = device->ram_base;
            return 1;
        case SYSTEM_INFO_RAM_SIZE_OFFSET:
            *value = device->ram_size;
            return 1;
        case SYSTEM_INFO_ROM_BASE_OFFSET:
            *value = device->rom_base;
            return 1;
        case SYSTEM_INFO_ROM_SIZE_OFFSET:
            *value = device->rom_size;
            return 1;
        case SYSTEM_INFO_RESET_VECTOR_OFFSET:
            *value = device->reset_vector;
            return 1;
        case SYSTEM_INFO_CORE_COUNT_OFFSET:
            *value = device->core_count;
            return 1;
        case SYSTEM_INFO_THREADS_PER_CORE_OFFSET:
            *value = device->threads_per_core;
            return 1;
        case SYSTEM_INFO_LOGICAL_PROCESSORS_OFFSET:
            *value = device->logical_processor_count;
            return 1;
        case SYSTEM_INFO_PHYSICAL_BITS_OFFSET:
            *value = device->physical_address_bits;
            return 1;
        case SYSTEM_INFO_PAGE_SIZE_OFFSET:
            *value = device->page_size;
            return 1;
        case SYSTEM_INFO_VIRTUAL_BITS_OFFSET:
            *value = device->virtual_address_bits;
            return 1;
        case SYSTEM_INFO_TIMER_FREQUENCY_OFFSET:
            *value = device->timer_frequency;
            return 1;
        case SYSTEM_INFO_INTERRUPT_LINES_OFFSET:
            *value = device->interrupt_line_count;
            return 1;
        case SYSTEM_INFO_VIO_HUB_BASE_OFFSET:
            *value = device->vio_hub_base;
            return 1;
        case SYSTEM_INFO_VIO_SLOT_COUNT_OFFSET:
            *value = device->vio_slot_count;
            return 1;
        case SYSTEM_INFO_DYNAMIC_MMIO_BASE_OFFSET:
            *value = device->dynamic_mmio_base;
            return 1;
        case SYSTEM_INFO_DYNAMIC_MMIO_SIZE_OFFSET:
            *value = device->dynamic_mmio_size;
            return 1;
        case SYSTEM_INFO_CONTROL_BASE_OFFSET:
            *value = device->system_control_base;
            return 1;
        case SYSTEM_INFO_ISA_VERSION_OFFSET:
            *value = SYSTEM_INFO_ISA_VERSION;
            return 1;
        case SYSTEM_INFO_EXTERNAL_IRQ_COUNT_OFFSET:
            *value = device->external_interrupt_line_count;
            return 1;
        case SYSTEM_INFO_IPI_BASE_OFFSET:
            *value = device->ipi_line_base;
            return 1;
        case SYSTEM_INFO_IRQ_CONTROLLER_BASE_OFFSET:
            *value = device->irq_controller_base;
            return 1;
        default:
            return 0;
    }
}

int system_info_device_init(SystemInfoDevice *device,
                            const SystemInfoDevice *configuration)
{
    if (device == NULL || configuration == NULL ||
        configuration->ram_size == 0 ||
        configuration->core_count == 0 ||
        configuration->threads_per_core == 0 ||
        configuration->logical_processor_count == 0) {
        return 0;
    }

    *device = *configuration;
    return 1;
}

BusDevice system_info_device_as_bus_device(SystemInfoDevice *device)
{
    BusDevice bus_device = {
        .context = device,
        .read = system_info_read,
        .write = NULL,
        .tick = NULL,
        .reset = NULL
    };
    return bus_device;
}
