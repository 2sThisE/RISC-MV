#include "bus.h"
#include "device_abi.h"
#include "device_manager.h"
#include "interrupt.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    VmDeviceHostApi host;
    VmDeviceResources resources;
    uint64_t values[VM_DEVICE_MAX_BARS];
    int irq_raised;
} FakeDevice;

static int fake_destroyed;
static int fake_logged;

static int fake_create(const VmDeviceHostApi *host,
                       const VmDeviceResources *resources,
                       const char *configuration,
                       void **device_context)
{
    assert(configuration != NULL);
    FakeDevice *device = malloc(sizeof(*device));
    if (device == NULL) {
        return 0;
    }
    *device = (FakeDevice){
        .host = *host,
        .resources = *resources
    };

    const uint8_t dma_data[2] = { UINT8_C(0xA5), UINT8_C(0x5A) };
    assert(device->host.dma_write(device->host.context,
                                  4,
                                  dma_data,
                                  sizeof(dma_data)));
    if (device->host.log != NULL) {
        device->host.log(device->host.context, 1, "fake device created");
    }
    *device_context = device;
    return 1;
}

static void fake_destroy(void *device_context)
{
    ++fake_destroyed;
    free(device_context);
}

static int fake_read(void *device_context,
                     uint32_t bar,
                     uint64_t offset,
                     uint32_t width,
                     uint64_t *value)
{
    FakeDevice *device = device_context;
    if (device == NULL || value == NULL ||
        bar >= VM_DEVICE_MAX_BARS || width != 8 ||
        offset != 0) {
        return 0;
    }
    *value = device->values[bar];
    return 1;
}

static int fake_write(void *device_context,
                      uint32_t bar,
                      uint64_t offset,
                      uint32_t width,
                      uint64_t value)
{
    FakeDevice *device = device_context;
    if (device == NULL || bar >= VM_DEVICE_MAX_BARS ||
        width != 8 || offset != 0) {
        return 0;
    }
    device->values[bar] = value;
    return 1;
}

static void fake_tick(void *device_context, uint64_t ticks)
{
    FakeDevice *device = device_context;
    device->values[0] += ticks;
    if (!device->irq_raised && device->values[0] >= 5) {
        assert(device->host.raise_irq(device->host.context,
                                      device->resources.irqs[3]));
        device->irq_raised = 1;
    }
}

static const VmDeviceModule FAKE_MODULE = {
    .abi_version = VM_DEVICE_ABI_VERSION,
    .struct_size = sizeof(VmDeviceModule),
    .descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .name = "fake-device",
        .device_class = VM_DEVICE_CLASS_TEST,
        .vendor_id = UINT32_C(0x1234),
        .device_id = UINT32_C(0x5678),
        .device_version = 1,
        .bar_count = 4,
        .irq_count = 4,
        .bar_sizes = {
            UINT64_C(32),
            UINT64_C(64),
            UINT64_C(16),
            UINT64_C(8)
        },
        .bar_alignments = {
            UINT64_C(4096),
            UINT64_C(64),
            UINT64_C(16),
            UINT64_C(8)
        }
    },
    .create = fake_create,
    .destroy = fake_destroy,
    .read = fake_read,
    .write = fake_write,
    .tick = fake_tick,
    .reset = NULL
};

static void capture_log(void *context,
                        uint32_t level,
                        const char *message)
{
    (void)context;
    assert(level == 1);
    assert(message != NULL);
    ++fake_logged;
}

int test_device_manager(void)
{
    uint8_t memory[64] = {0};
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
    device_manager_set_log_callback(&manager, capture_log, NULL);
    assert(bus_map_device(&bus,
                          VIO_HUB_MMIO_BASE,
                          VIO_HUB_MMIO_SIZE,
                          device_manager_hub_as_bus_device(&manager)));

    VmDeviceDescriptor fixed_descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .name = "fixed-test",
        .device_class = VM_DEVICE_CLASS_SYSTEM,
        .vendor_id = 1,
        .device_id = 2,
        .device_version = 1,
        .bar_count = 1,
        .irq_count = 0,
        .bar_sizes = { UINT64_C(16) },
        .bar_alignments = { UINT64_C(8) }
    };
    VmDeviceResources fixed_resources = {
        .struct_size = sizeof(VmDeviceResources),
        .bar_count = 1,
        .irq_count = 0,
        .bar_bases = { UINT64_C(0xFFFFFFFFFFFF8000) },
        .bar_sizes = { UINT64_C(16) },
        .irqs = { UINT32_MAX }
    };
    size_t fixed_slot;
    assert(device_manager_publish_fixed(&manager,
                                        &fixed_descriptor,
                                        &fixed_resources,
                                        &fixed_slot));
    assert(fixed_slot == 0);

    size_t external_slot;
    assert(device_manager_attach_module(&manager,
                                        &FAKE_MODULE,
                                        "test=true",
                                        &external_slot));
    assert(external_slot == 1);
    assert(fake_logged == 1);
    assert(memory[4] == UINT8_C(0xA5));
    assert(memory[5] == UINT8_C(0x5A));

    uint64_t value;
    assert(bus_read(&bus,
                    VIO_HUB_MMIO_BASE + VIO_HUB_MAGIC_OFFSET,
                    8,
                    &value));
    assert(value == VIO_HUB_MAGIC);
    assert(bus_read(&bus,
                    VIO_HUB_MMIO_BASE + VIO_HUB_GENERATION_OFFSET,
                    8,
                    &value));
    assert(value == 2);

    uint64_t slot_base = VIO_HUB_MMIO_BASE + VIO_HUB_SLOT_BASE +
                         external_slot * VIO_HUB_SLOT_STRIDE;
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == (VIO_SLOT_STATUS_PRESENT |
                     VIO_SLOT_STATUS_EXTERNAL));
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_CLASS_OFFSET,
                    8,
                    &value));
    assert(value == VM_DEVICE_CLASS_TEST);
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_BAR0_BASE_OFFSET,
                    8,
                    &value));
    uint64_t device_bases[VM_DEVICE_MAX_BARS] = { value, 0, 0, 0 };
    const uint64_t bar_base_offsets[VM_DEVICE_MAX_BARS] = {
        VIO_SLOT_BAR0_BASE_OFFSET,
        VIO_SLOT_BAR1_BASE_OFFSET,
        VIO_SLOT_BAR2_BASE_OFFSET,
        VIO_SLOT_BAR3_BASE_OFFSET
    };
    const uint64_t bar_size_offsets[VM_DEVICE_MAX_BARS] = {
        VIO_SLOT_BAR0_SIZE_OFFSET,
        VIO_SLOT_BAR1_SIZE_OFFSET,
        VIO_SLOT_BAR2_SIZE_OFFSET,
        VIO_SLOT_BAR3_SIZE_OFFSET
    };
    const uint64_t irq_offsets[VM_DEVICE_MAX_IRQS] = {
        VIO_SLOT_IRQ0_OFFSET,
        VIO_SLOT_IRQ1_OFFSET,
        VIO_SLOT_IRQ2_OFFSET,
        VIO_SLOT_IRQ3_OFFSET
    };
    const uint64_t expected_sizes[VM_DEVICE_MAX_BARS] = {
        UINT64_C(32), UINT64_C(64), UINT64_C(16), UINT64_C(8)
    };
    assert(device_bases[0] == DEVICE_MANAGER_DYNAMIC_MMIO_BASE);
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_BAR_COUNT_OFFSET,
                    8,
                    &value));
    assert(value == VM_DEVICE_MAX_BARS);
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_IRQ_COUNT_OFFSET,
                    8,
                    &value));
    assert(value == VM_DEVICE_MAX_IRQS);
    for (uint32_t bar = 0; bar < VM_DEVICE_MAX_BARS; ++bar) {
        assert(bus_read(&bus,
                        slot_base + bar_base_offsets[bar],
                        8,
                        &device_bases[bar]));
        assert(bus_read(&bus,
                        slot_base + bar_size_offsets[bar],
                        8,
                        &value));
        assert(value == expected_sizes[bar]);
        assert((device_bases[bar] &
                (FAKE_MODULE.descriptor.bar_alignments[bar] - 1)) == 0);
        assert(bus_write(&bus, device_bases[bar], 8, bar + 10));
        assert(bus_read(&bus, device_bases[bar], 8, &value));
        assert(value == bar + 10);
    }
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_IRQ0_OFFSET,
                    8,
                    &value));
    assert(value == 2);
    for (uint32_t irq = 0; irq < VM_DEVICE_MAX_IRQS; ++irq) {
        assert(bus_read(&bus,
                        slot_base + irq_offsets[irq],
                        8,
                        &value));
        assert(value == irq + 2);
    }

    assert(bus_write(&bus, device_bases[0], 8, 3));
    assert(bus_read(&bus, device_bases[0], 8, &value));
    assert(value == 3);
    bus_tick(&bus, 2, &router);
    assert(bus_read(&bus, device_bases[0], 8, &value));
    assert(value == 5);

    unsigned int line;
    assert(interrupt_controller_take_next(&controller, &line));
    assert(line == 5);

    assert(bus_read(&bus,
                    VIO_HUB_MMIO_BASE + VIO_HUB_EVENT_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VIO_HUB_EVENT_DEVICE_ADDED) != 0);
    assert(bus_write(&bus,
                     VIO_HUB_MMIO_BASE + VIO_HUB_EVENT_ACK_OFFSET,
                     8,
                     VIO_HUB_EVENT_DEVICE_ADDED));
    assert(bus_read(&bus,
                    VIO_HUB_MMIO_BASE + VIO_HUB_EVENT_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == 0);

    assert(device_manager_detach_module(&manager, external_slot));
    assert(fake_destroyed == 1);
    for (uint32_t bar = 0; bar < VM_DEVICE_MAX_BARS; ++bar) {
        assert(!bus_read(&bus, device_bases[bar], 8, &value));
    }
    assert(bus_read(&bus,
                    VIO_HUB_MMIO_BASE + VIO_HUB_GENERATION_OFFSET,
                    8,
                    &value));
    assert(value == 3);
    assert(bus_read(&bus,
                    slot_base + VIO_SLOT_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == 0);
    assert(bus_read(&bus,
                    VIO_HUB_MMIO_BASE + VIO_HUB_EVENT_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VIO_HUB_EVENT_DEVICE_REMOVED) != 0);

    assert(device_manager_attach_module(&manager,
                                        &FAKE_MODULE,
                                        "test=true",
                                        &external_slot));
    assert(external_slot == 1);
    const DeviceManagerSlot *reattached = device_manager_slot(
        &manager,
        external_slot);
    assert(reattached != NULL);
    assert(reattached->resources.bar_bases[0] == device_bases[0]);
    assert(reattached->resources.irqs[0] == 2);
    assert(bus_write(&bus, reattached->resources.bar_bases[0], 8, 4));
    bus_tick(&bus, 1, &router);
    assert(device_manager_detach_module(&manager, external_slot));
    assert(!interrupt_controller_take_next(&controller, &line));

    device_manager_destroy(&manager);
    assert(fake_destroyed == 2);
    return 0;
}
