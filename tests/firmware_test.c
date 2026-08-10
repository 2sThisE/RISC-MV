#include "firmware.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

int test_firmware(void)
{
    CvmFirmwareTable table;
    memset(&table, 0, sizeof(table));
    memcpy(table.magic, CVM_FIRMWARE_MAGIC, 8);
    table.abi_major = CVM_FIRMWARE_ABI_MAJOR;
    table.abi_minor = CVM_FIRMWARE_ABI_MINOR;
    table.header_size = CVM_FIRMWARE_TABLE_SIZE;
    table.total_size = CVM_FIRMWARE_TABLE_SIZE;

    assert(sizeof(table) == 256);
    assert(memcmp(table.magic, "CVMFW001", 8) == 0);
    assert(CVM_FIRMWARE_HANDOFF_MAGIC ==
           UINT64_C(0x31304857464D5643));
    assert(offsetof(CvmFirmwareTable, ram_size) == 0x20);
    assert(offsetof(CvmFirmwareTable, boot_device_slot) == 0x80);
    assert(offsetof(CvmFirmwareTable, console_write) == 0xA0);
    assert(offsetof(CvmFirmwareTable, reset_system) == 0xC8);
    return 0;
}
