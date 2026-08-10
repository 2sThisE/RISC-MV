#include "block_protocol.h"
#include "bus.h"
#include "device_manager.h"
#include "block_device.h"
#include "host_thread.h"
#include "interrupt.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_TEST_IMAGE "block_device_test.img"

static uint64_t wait_for_status(Bus *bus,
                                uint64_t base,
                                uint64_t wanted)
{
    for (unsigned int attempt = 0; attempt < 3000; ++attempt) {
        uint64_t status;
        assert(bus_read(bus,
                        base + VM_BLOCK_STATUS_OFFSET,
                        8,
                        &status));
        if ((status & wanted) != 0) {
            return status;
        }
        host_thread_sleep_milliseconds(1);
    }
    assert(!"block request timed out");
    return 0;
}

static unsigned int wait_for_irq(InterruptController *controller)
{
    for (unsigned int attempt = 0; attempt < 3000; ++attempt) {
        unsigned int line;
        if (interrupt_controller_take_next(controller, &line)) {
            return line;
        }
        host_thread_sleep_milliseconds(1);
    }
    assert(!"block interrupt timed out");
    return UINT32_MAX;
}

static uint64_t attach_block(DeviceManager *manager,
                             Bus *bus,
                             const char *configuration,
                             size_t *slot_index)
{
    assert(device_manager_attach_module(manager,
                                        vm_block_device_module(),
                                        configuration,
                                        slot_index));
    const DeviceManagerSlot *slot = device_manager_slot(manager,
                                                        *slot_index);
    assert(slot != NULL);
    assert(slot->descriptor.device_class == VM_DEVICE_CLASS_STORAGE);
    assert(slot->resources.irq_count == 1);
    uint64_t base = slot->resources.bar_bases[0];
    uint64_t value;
    assert(bus_read(bus, base + VM_BLOCK_MAGIC_OFFSET, 8, &value));
    assert(value == VM_BLOCK_MAGIC);
    assert(bus_read(bus, base + VM_BLOCK_SECTOR_SIZE_OFFSET, 8, &value));
    assert(value == VM_BLOCK_SECTOR_SIZE);
    return base;
}

static void submit_io(Bus *bus,
                      uint64_t base,
                      uint64_t command,
                      uint64_t lba,
                      uint64_t dma_address,
                      uint64_t sector_count)
{
    assert(bus_write(bus, base + VM_BLOCK_LBA_OFFSET, 8, lba));
    assert(bus_write(bus,
                     base + VM_BLOCK_DMA_ADDRESS_OFFSET,
                     8,
                     dma_address));
    assert(bus_write(bus,
                     base + VM_BLOCK_SECTOR_COUNT_OFFSET,
                     8,
                     sector_count));
    assert(bus_write(bus,
                     base + VM_BLOCK_COMMAND_OFFSET,
                     8,
                     command));
}

static void test_read_write_and_flush(void)
{
    (void)remove(BLOCK_TEST_IMAGE);
    uint8_t memory[2048] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    assert(ram_enable_synchronization(&ram));
    Bus bus;
    assert(bus_init(&bus, &ram));
    InterruptController controller;
    interrupt_controller_init(&controller);
    InterruptController *targets[1] = { &controller };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 1));
    DeviceManager manager;
    assert(device_manager_init(&manager, &bus, &router));

    size_t slot_index;
    uint64_t base = attach_block(&manager,
                                 &bus,
                                 "path=block_device_test.img;create=4096",
                                 &slot_index);
    const DeviceManagerSlot *slot = device_manager_slot(&manager,
                                                        slot_index);
    assert(slot != NULL);
    unsigned int expected_irq = slot->resources.irqs[0];
    uint64_t value;
    assert(bus_read(&bus, base + VM_BLOCK_CAPACITY_OFFSET, 8, &value));
    assert(value == 8);
    assert(bus_write(&bus,
                     base + VM_BLOCK_CONTROL_OFFSET,
                     8,
                     VM_BLOCK_CONTROL_IRQ_ENABLE));

    uint8_t pattern[VM_BLOCK_SECTOR_SIZE];
    for (size_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = (uint8_t)(i ^ UINT8_C(0xA5));
    }
    assert(ram_write_block(&ram, 0x100, pattern, sizeof(pattern)));
    submit_io(&bus, base, VM_BLOCK_COMMAND_WRITE, 2, 0x100, 1);
    uint64_t status = wait_for_status(&bus,
                                      base,
                                      VM_BLOCK_STATUS_DONE |
                                          VM_BLOCK_STATUS_ERROR);
    assert((status & VM_BLOCK_STATUS_DONE) != 0);
    assert(wait_for_irq(&controller) == expected_irq);
    assert(bus_write(&bus,
                     base + VM_BLOCK_IRQ_ACK_OFFSET,
                     8,
                     VM_BLOCK_IRQ_COMPLETE));

    uint8_t zeros[VM_BLOCK_SECTOR_SIZE] = {0};
    assert(ram_write_block(&ram, 0x400, zeros, sizeof(zeros)));
    submit_io(&bus, base, VM_BLOCK_COMMAND_READ, 2, 0x400, 1);
    status = wait_for_status(&bus,
                             base,
                             VM_BLOCK_STATUS_DONE |
                                 VM_BLOCK_STATUS_ERROR);
    assert((status & VM_BLOCK_STATUS_DONE) != 0);
    assert(wait_for_irq(&controller) == expected_irq);
    uint8_t result[VM_BLOCK_SECTOR_SIZE];
    assert(ram_read_block(&ram, 0x400, result, sizeof(result)));
    assert(memcmp(result, pattern, sizeof(result)) == 0);
    assert(bus_write(&bus,
                     base + VM_BLOCK_IRQ_ACK_OFFSET,
                     8,
                     VM_BLOCK_IRQ_COMPLETE));

    assert(bus_write(&bus,
                     base + VM_BLOCK_COMMAND_OFFSET,
                     8,
                     VM_BLOCK_COMMAND_FLUSH));
    status = wait_for_status(&bus,
                             base,
                             VM_BLOCK_STATUS_DONE |
                                 VM_BLOCK_STATUS_ERROR);
    assert((status & VM_BLOCK_STATUS_DONE) != 0);
    assert(wait_for_irq(&controller) == expected_irq);
    assert(bus_write(&bus,
                     base + VM_BLOCK_IRQ_ACK_OFFSET,
                     8,
                     VM_BLOCK_IRQ_COMPLETE));

    submit_io(&bus, base, VM_BLOCK_COMMAND_READ, 8, 0x400, 1);
    assert(bus_read(&bus, base + VM_BLOCK_STATUS_OFFSET, 8, &status));
    assert((status & VM_BLOCK_STATUS_ERROR) != 0);
    assert(bus_read(&bus, base + VM_BLOCK_ERROR_OFFSET, 8, &value));
    assert(value == VM_BLOCK_ERROR_RANGE);
    assert(wait_for_irq(&controller) == expected_irq);

    device_manager_destroy(&manager);

    FILE *file = fopen(BLOCK_TEST_IMAGE, "rb");
    assert(file != NULL);
    assert(fseek(file, 2 * (long)VM_BLOCK_SECTOR_SIZE, SEEK_SET) == 0);
    assert(fread(result, 1, sizeof(result), file) == sizeof(result));
    assert(fclose(file) == 0);
    assert(memcmp(result, pattern, sizeof(result)) == 0);
}

static void test_read_only_rejects_write(void)
{
    uint8_t memory[1024] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));
    InterruptController controller;
    interrupt_controller_init(&controller);
    InterruptController *targets[1] = { &controller };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 1));
    DeviceManager manager;
    assert(device_manager_init(&manager, &bus, &router));

    size_t slot_index;
    uint64_t base = attach_block(&manager,
                                 &bus,
                                 "path=block_device_test.img;readonly=true",
                                 &slot_index);
    uint64_t value;
    assert(bus_read(&bus, base + VM_BLOCK_FEATURES_OFFSET, 8, &value));
    assert((value & VM_BLOCK_FEATURE_READ_ONLY) != 0);
    submit_io(&bus, base, VM_BLOCK_COMMAND_WRITE, 0, 0, 1);
    assert(bus_read(&bus, base + VM_BLOCK_STATUS_OFFSET, 8, &value));
    assert((value & VM_BLOCK_STATUS_ERROR) != 0);
    assert((value & VM_BLOCK_STATUS_READ_ONLY) != 0);
    assert(bus_read(&bus, base + VM_BLOCK_ERROR_OFFSET, 8, &value));
    assert(value == VM_BLOCK_ERROR_READ_ONLY);
    device_manager_destroy(&manager);
}

int test_block_device(void)
{
    test_read_write_and_flush();
    test_read_only_rejects_write();
    assert(remove(BLOCK_TEST_IMAGE) == 0);
    return 0;
}
