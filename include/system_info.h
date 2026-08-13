#ifndef SYSTEM_INFO_H
#define SYSTEM_INFO_H

#include <stdint.h>

#include "bus.h"
#include "system_info_protocol.h"

typedef struct {
    uint64_t features;
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t rom_base;
    uint64_t rom_size;
    uint64_t reset_vector;
    uint64_t core_count;
    uint64_t threads_per_core;
    uint64_t logical_processor_count;
    uint64_t physical_address_bits;
    uint64_t page_size;
    uint64_t virtual_address_bits;
    uint64_t timer_frequency;
    uint64_t interrupt_line_count;
    uint64_t vio_hub_base;
    uint64_t vio_slot_count;
    uint64_t dynamic_mmio_base;
    uint64_t dynamic_mmio_size;
    uint64_t system_control_base;
    uint64_t external_interrupt_line_count;
    uint64_t ipi_line_base;
    uint64_t irq_controller_base;
} SystemInfoDevice;

int system_info_device_init(SystemInfoDevice *device,
                            const SystemInfoDevice *configuration);
BusDevice system_info_device_as_bus_device(SystemInfoDevice *device);

#endif
