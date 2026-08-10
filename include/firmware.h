#ifndef CVM_FIRMWARE_H
#define CVM_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#define CVM_FIRMWARE_MAGIC "CVMFW001"
#define CVM_FIRMWARE_HANDOFF_MAGIC UINT64_C(0x31304857464D5643)
#define CVM_FIRMWARE_ABI_MAJOR UINT16_C(1)
#define CVM_FIRMWARE_ABI_MINOR UINT16_C(0)
#define CVM_FIRMWARE_TABLE_SIZE UINT32_C(256)

enum {
    CVM_FIRMWARE_OK = 0,
    CVM_FIRMWARE_ERROR = 1
};

/* Guest physical addresses of callable Boot ROM service entry points are
   stored as uint64_t values. They are invoked with CALLR using the firmware
   calling convention documented in docs/FIRMWARE_ABI.md. */
typedef struct {
    uint8_t magic[8];
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t header_size;
    uint32_t total_size;
    uint32_t flags;
    uint32_t checksum;
    uint32_t reserved0;
    uint64_t ram_size;
    uint64_t cpu_features;
    uint64_t page_size;
    uint64_t physical_address_bits;
    uint64_t virtual_address_bits;
    uint64_t vio_hub_base;
    uint64_t system_info_base;
    uint64_t system_control_base;
    uint64_t irq_controller_base;
    uint64_t core_control_base;
    uint64_t timer_base;
    uint64_t uart_base;
    uint32_t boot_device_slot;
    uint32_t boot_partition_index;
    uint64_t boot_partition_lba;
    uint64_t boot_partition_sectors;
    uint64_t memory_map_key;
    uint64_t console_write;
    uint64_t get_memory_map;
    uint64_t get_file_size;
    uint64_t read_file;
    uint64_t exit_boot_services;
    uint64_t reset_system;
    uint8_t reserved[48];
} CvmFirmwareTable;

_Static_assert(sizeof(CvmFirmwareTable) == CVM_FIRMWARE_TABLE_SIZE,
               "CvmFirmwareTable layout changed");
_Static_assert(offsetof(CvmFirmwareTable, console_write) == 0xA0,
               "CvmFirmwareTable service layout changed");
_Static_assert(offsetof(CvmFirmwareTable, exit_boot_services) == 0xC0,
               "CvmFirmwareTable exit layout changed");

#endif
