#include "device_abi.h"

#include <stddef.h>
#include <stdlib.h>

#define COUNTER_VALUE_OFFSET   UINT64_C(0)
#define COUNTER_STEP_OFFSET    UINT64_C(8)
#define COUNTER_COMPARE_OFFSET UINT64_C(16)
#define COUNTER_CONTROL_OFFSET UINT64_C(24)
#define COUNTER_STATUS_OFFSET  UINT64_C(32)
#define COUNTER_MMIO_SIZE      UINT64_C(40)

#define COUNTER_CONTROL_ENABLE     (UINT64_C(1) << 0)
#define COUNTER_CONTROL_IRQ_ENABLE (UINT64_C(1) << 1)
#define COUNTER_STATUS_PENDING     (UINT64_C(1) << 0)

typedef struct {
    VmDeviceHostApi host;
    VmDeviceResources resources;
    uint64_t value;
    uint64_t step;
    uint64_t compare;
    uint64_t control;
    uint64_t status;
} CounterDevice;

static int counter_create(const VmDeviceHostApi *host,
                          const VmDeviceResources *resources,
                          const char *configuration,
                          void **device_context)
{
    (void)configuration;
    if (host == NULL || resources == NULL || device_context == NULL ||
        host->abi_version != VM_DEVICE_ABI_VERSION ||
        host->struct_size < sizeof(*host) ||
        resources->struct_size < sizeof(*resources) ||
        resources->bar_count != 1 || resources->irq_count != 1) {
        return 0;
    }

    CounterDevice *device = malloc(sizeof(*device));
    if (device == NULL) {
        return 0;
    }
    *device = (CounterDevice){
        .host = *host,
        .resources = *resources,
        .step = 1
    };
    *device_context = device;
    if (device->host.log != NULL) {
        device->host.log(device->host.context,
                         1,
                         "sample counter device attached");
    }
    return 1;
}

static void counter_destroy(void *device_context)
{
    free(device_context);
}

static int counter_read(void *device_context,
                        uint32_t bar,
                        uint64_t offset,
                        uint32_t width,
                        uint64_t *value)
{
    CounterDevice *device = device_context;
    if (device == NULL || value == NULL || bar != 0 || width != 8) {
        return 0;
    }

    switch (offset) {
        case COUNTER_VALUE_OFFSET:
            *value = device->value;
            return 1;
        case COUNTER_STEP_OFFSET:
            *value = device->step;
            return 1;
        case COUNTER_COMPARE_OFFSET:
            *value = device->compare;
            return 1;
        case COUNTER_CONTROL_OFFSET:
            *value = device->control;
            return 1;
        case COUNTER_STATUS_OFFSET:
            *value = device->status;
            return 1;
        default:
            return 0;
    }
}

static int counter_write(void *device_context,
                         uint32_t bar,
                         uint64_t offset,
                         uint32_t width,
                         uint64_t value)
{
    CounterDevice *device = device_context;
    if (device == NULL || bar != 0 || width != 8) {
        return 0;
    }

    switch (offset) {
        case COUNTER_VALUE_OFFSET:
            device->value = value;
            return 1;
        case COUNTER_STEP_OFFSET:
            device->step = value;
            return 1;
        case COUNTER_COMPARE_OFFSET:
            device->compare = value;
            return 1;
        case COUNTER_CONTROL_OFFSET:
            device->control = value &
                (COUNTER_CONTROL_ENABLE | COUNTER_CONTROL_IRQ_ENABLE);
            return 1;
        case COUNTER_STATUS_OFFSET:
            if ((value & COUNTER_STATUS_PENDING) != 0) {
                device->status &= ~COUNTER_STATUS_PENDING;
            }
            return 1;
        default:
            return 0;
    }
}

static void counter_tick(void *device_context, uint64_t ticks)
{
    CounterDevice *device = device_context;
    if (device == NULL || ticks == 0 ||
        (device->control & COUNTER_CONTROL_ENABLE) == 0) {
        return;
    }

    device->value += device->step * ticks;
    if (device->compare == 0 || device->value < device->compare) {
        return;
    }

    device->status |= COUNTER_STATUS_PENDING;
    device->control &= ~COUNTER_CONTROL_ENABLE;
    if ((device->control & COUNTER_CONTROL_IRQ_ENABLE) != 0 &&
        device->host.raise_irq != NULL) {
        (void)device->host.raise_irq(device->host.context,
                                     device->resources.irqs[0]);
    }
}

static void counter_reset(void *device_context)
{
    CounterDevice *device = device_context;
    if (device == NULL) {
        return;
    }
    device->value = 0;
    device->step = 1;
    device->compare = 0;
    device->control = 0;
    device->status = 0;
}

static const VmDeviceModule COUNTER_MODULE = {
    .abi_version = VM_DEVICE_ABI_VERSION,
    .struct_size = sizeof(VmDeviceModule),
    .descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .name = "sample-counter",
        .device_class = VM_DEVICE_CLASS_TEST,
        .vendor_id = UINT32_C(0x564D),
        .device_id = UINT32_C(0x0001),
        .device_version = 1,
        .features = 0,
        .bar_count = 1,
        .irq_count = 1,
        .bar_sizes = { COUNTER_MMIO_SIZE },
        .bar_alignments = { UINT64_C(4096) }
    },
    .create = counter_create,
    .destroy = counter_destroy,
    .read = counter_read,
    .write = counter_write,
    .tick = counter_tick,
    .reset = counter_reset
};

#ifdef _WIN32
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const VmDeviceModule *vm_device_query(uint32_t host_abi_version)
{
    return host_abi_version == VM_DEVICE_ABI_VERSION
               ? &COUNTER_MODULE
               : NULL;
}
