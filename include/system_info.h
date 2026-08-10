#ifndef SYSTEM_INFO_H
#define SYSTEM_INFO_H

#include <stdint.h>

#include "bus.h"

#define SYSTEM_INFO_MMIO_BASE UINT64_C(0xFFFFFFFFFFFC1000)
#define SYSTEM_INFO_MMIO_SIZE UINT64_C(0x100)

#define SYSTEM_INFO_MAGIC UINT64_C(0x464E4953)
#define SYSTEM_INFO_VERSION UINT64_C(1)
#define SYSTEM_INFO_ISA_VERSION UINT64_C(1)

#define SYSTEM_INFO_MAGIC_OFFSET              UINT64_C(0x00)
#define SYSTEM_INFO_VERSION_OFFSET            UINT64_C(0x08)
#define SYSTEM_INFO_FEATURES_OFFSET           UINT64_C(0x10)
#define SYSTEM_INFO_RAM_BASE_OFFSET           UINT64_C(0x18)
#define SYSTEM_INFO_RAM_SIZE_OFFSET           UINT64_C(0x20)
#define SYSTEM_INFO_ROM_BASE_OFFSET           UINT64_C(0x28)
#define SYSTEM_INFO_ROM_SIZE_OFFSET           UINT64_C(0x30)
#define SYSTEM_INFO_RESET_VECTOR_OFFSET       UINT64_C(0x38)
#define SYSTEM_INFO_CORE_COUNT_OFFSET         UINT64_C(0x40)
#define SYSTEM_INFO_THREADS_PER_CORE_OFFSET   UINT64_C(0x48)
#define SYSTEM_INFO_LOGICAL_PROCESSORS_OFFSET UINT64_C(0x50)
#define SYSTEM_INFO_PHYSICAL_BITS_OFFSET      UINT64_C(0x58)
#define SYSTEM_INFO_PAGE_SIZE_OFFSET          UINT64_C(0x60)
#define SYSTEM_INFO_VIRTUAL_BITS_OFFSET       UINT64_C(0x68)
#define SYSTEM_INFO_TIMER_FREQUENCY_OFFSET    UINT64_C(0x70)
#define SYSTEM_INFO_INTERRUPT_LINES_OFFSET    UINT64_C(0x78)
#define SYSTEM_INFO_VIO_HUB_BASE_OFFSET       UINT64_C(0x80)
#define SYSTEM_INFO_VIO_SLOT_COUNT_OFFSET     UINT64_C(0x88)
#define SYSTEM_INFO_DYNAMIC_MMIO_BASE_OFFSET  UINT64_C(0x90)
#define SYSTEM_INFO_DYNAMIC_MMIO_SIZE_OFFSET  UINT64_C(0x98)
#define SYSTEM_INFO_CONTROL_BASE_OFFSET       UINT64_C(0xA0)
#define SYSTEM_INFO_ISA_VERSION_OFFSET        UINT64_C(0xA8)
#define SYSTEM_INFO_EXTERNAL_IRQ_COUNT_OFFSET UINT64_C(0xB0)
#define SYSTEM_INFO_IPI_BASE_OFFSET           UINT64_C(0xB8)
#define SYSTEM_INFO_IRQ_CONTROLLER_BASE_OFFSET UINT64_C(0xC0)

#define SYSTEM_INFO_FEATURE_MMU            (UINT64_C(1) << 0)
#define SYSTEM_INFO_FEATURE_MULTICORE      (UINT64_C(1) << 1)
#define SYSTEM_INFO_FEATURE_INTERRUPTS     (UINT64_C(1) << 2)
#define SYSTEM_INFO_FEATURE_ATOMICS        (UINT64_C(1) << 3)
#define SYSTEM_INFO_FEATURE_SIMD           (UINT64_C(1) << 4)
#define SYSTEM_INFO_FEATURE_FLOATING_POINT (UINT64_C(1) << 5)
#define SYSTEM_INFO_FEATURE_BOOT_ROM       (UINT64_C(1) << 6)
#define SYSTEM_INFO_FEATURE_VIO            (UINT64_C(1) << 7)
#define SYSTEM_INFO_FEATURE_SYSTEM_CONTROL (UINT64_C(1) << 8)
#define SYSTEM_INFO_FEATURE_IRQ_CONTROLLER  (UINT64_C(1) << 9)

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
