#include "bus.h"
#include "device_abi.h"
#include "device_manager.h"
#include "display_host.h"
#include "display_protocol.h"
#include "headless_display.h"
#include "interrupt.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

const VmDeviceModule *vm_device_query(uint32_t host_abi_version);

int test_display(void)
{
    uint8_t memory[4096] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    const uint8_t pixels[16] = {
        0x00, 0x00, 0xFF, 0x00,
        0x00, 0xFF, 0x00, 0x00,
        0xFF, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0x00
    };
    const uint64_t framebuffer = 257;
    assert(ram_write_block(&ram,
                           framebuffer,
                           pixels,
                           sizeof(pixels)));
    uint8_t block_copy[sizeof(pixels)] = {0};
    assert(ram_read_block(&ram,
                          framebuffer,
                          block_copy,
                          sizeof(block_copy)));
    assert(memcmp(block_copy, pixels, sizeof(pixels)) == 0);
    assert(!ram_read_block(&ram, sizeof(memory) - 4, block_copy, 8));

    InterruptController controller;
    interrupt_controller_init(&controller);
    InterruptController *targets[1] = { &controller };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 1));

    DeviceManager manager;
    assert(device_manager_init(&manager, &bus, &router));
    HeadlessDisplay frontend;
    assert(headless_display_init(&frontend));
    assert(device_manager_register_service(
        &manager,
        VM_DISPLAY_SERVICE_NAME,
        VM_DISPLAY_HOST_VERSION,
        headless_display_host(&frontend)));
    assert(!device_manager_register_service(
        &manager,
        VM_DISPLAY_SERVICE_NAME,
        VM_DISPLAY_HOST_VERSION,
        headless_display_host(&frontend)));

    const VmDeviceModule *module = vm_device_query(
        VM_DEVICE_ABI_VERSION);
    assert(module != NULL);
    size_t slot_index;
    assert(device_manager_attach_module(&manager,
                                        module,
                                        "",
                                        &slot_index));
    const DeviceManagerSlot *slot = device_manager_slot(&manager,
                                                        slot_index);
    assert(slot != NULL);
    assert(slot->descriptor.device_class == VM_DEVICE_CLASS_DISPLAY);
    assert(slot->resources.irq_count == 1);
    uint64_t base = slot->resources.bar_bases[0];

    assert(bus_write(&bus,
                     base + DISPLAY_CONTROL_OFFSET,
                     8,
                     DISPLAY_CONTROL_ENABLE |
                         DISPLAY_CONTROL_IRQ_ENABLE));
    assert(bus_write(&bus, base + DISPLAY_WIDTH_OFFSET, 8, 2));
    assert(bus_write(&bus, base + DISPLAY_HEIGHT_OFFSET, 8, 2));
    assert(bus_write(&bus, base + DISPLAY_STRIDE_OFFSET, 8, 8));
    assert(bus_write(&bus,
                     base + DISPLAY_FORMAT_OFFSET,
                     8,
                     VM_PIXEL_FORMAT_XRGB8888));
    assert(bus_write(&bus,
                     base + DISPLAY_FRAMEBUFFER_OFFSET,
                     8,
                     framebuffer));
    assert(bus_write(&bus,
                     base + DISPLAY_BUFFER_SIZE_OFFSET,
                     8,
                     sizeof(pixels)));
    assert(bus_write(&bus,
                     base + DISPLAY_COMMAND_OFFSET,
                     8,
                     DISPLAY_COMMAND_SET_MODE));
    uint64_t value;
    assert(bus_read(&bus,
                    base + DISPLAY_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == DISPLAY_STATUS_READY);
    assert(bus_read(&bus,
                    base + DISPLAY_CAPABILITIES_OFFSET,
                    8,
                    &value));
    assert((value & DISPLAY_CAP_MODE_SET) != 0);
    assert(bus_write(&bus,
                     base + DISPLAY_COMMAND_OFFSET,
                     8,
                     DISPLAY_COMMAND_PRESENT));

    size_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frame_number;
    assert(headless_display_frame_info(&frontend,
                                       &frame_size,
                                       &width,
                                       &height,
                                       &stride,
                                       &format,
                                       &frame_number));
    assert(frame_size == sizeof(pixels));
    assert(width == 2 && height == 2 && stride == 8);
    assert(format == VM_PIXEL_FORMAT_XRGB8888);
    assert(frame_number == 1);
    uint8_t captured[sizeof(pixels)] = {0};
    size_t copied;
    assert(headless_display_copy_frame(&frontend,
                                       captured,
                                       sizeof(captured),
                                       &copied));
    assert(copied == sizeof(pixels));
    assert(memcmp(captured, pixels, sizeof(pixels)) == 0);

    unsigned int line;
    assert(interrupt_controller_take_next(&controller, &line));
    assert(line == slot->resources.irqs[0]);
    assert(bus_read(&bus,
                    base + DISPLAY_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == DISPLAY_STATUS_READY);
    assert(bus_read(&bus,
                    base + DISPLAY_FRAME_NUMBER_OFFSET,
                    8,
                    &value));
    assert(value == 1);
    assert(bus_read(&bus,
                    base + DISPLAY_IRQ_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & DISPLAY_IRQ_PRESENT_COMPLETE) != 0);
    assert(bus_write(&bus,
                     base + DISPLAY_IRQ_STATUS_OFFSET,
                     8,
                     DISPLAY_IRQ_PRESENT_COMPLETE));

    /* 잘못된 stride는 Bus fault가 아니라 장치 오류와 IRQ가 된다. */
    assert(bus_write(&bus, base + DISPLAY_STRIDE_OFFSET, 8, 4));
    assert(bus_write(&bus,
                     base + DISPLAY_COMMAND_OFFSET,
                     8,
                     DISPLAY_COMMAND_PRESENT));
    assert(interrupt_controller_take_next(&controller, &line));
    assert(bus_read(&bus,
                    base + DISPLAY_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & DISPLAY_STATUS_ERROR) != 0);
    assert(bus_read(&bus,
                    base + DISPLAY_ERROR_CODE_OFFSET,
                    8,
                    &value));
    assert(value == DISPLAY_ERROR_STRIDE);
    assert(bus_write(&bus,
                     base + DISPLAY_STATUS_OFFSET,
                     8,
                     DISPLAY_STATUS_ERROR));
    assert(bus_read(&bus,
                    base + DISPLAY_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == DISPLAY_STATUS_READY);
    assert(bus_write(&bus,
                     base + DISPLAY_IRQ_STATUS_OFFSET,
                     8,
                     DISPLAY_IRQ_ERROR));

    /* SET_MODE validates dimensions independently of framebuffer DMA and
     * leaves the device usable after the error is acknowledged. */
    assert(bus_write(&bus,
                     base + DISPLAY_WIDTH_OFFSET,
                     8,
                     DISPLAY_MAX_WIDTH + 1));
    assert(bus_write(&bus,
                     base + DISPLAY_STRIDE_OFFSET,
                     8,
                     (DISPLAY_MAX_WIDTH + 1) * 4));
    assert(bus_write(&bus,
                     base + DISPLAY_COMMAND_OFFSET,
                     8,
                     DISPLAY_COMMAND_SET_MODE));
    assert(interrupt_controller_take_next(&controller, &line));
    assert(bus_read(&bus,
                    base + DISPLAY_ERROR_CODE_OFFSET,
                    8,
                    &value));
    assert(value == DISPLAY_ERROR_DIMENSIONS);
    assert(bus_write(&bus,
                     base + DISPLAY_STATUS_OFFSET,
                     8,
                     DISPLAY_STATUS_ERROR));
    assert(bus_write(&bus,
                     base + DISPLAY_IRQ_STATUS_OFFSET,
                     8,
                     DISPLAY_IRQ_ERROR));

    device_manager_destroy(&manager);
    headless_display_destroy(&frontend);
    return 0;
}
