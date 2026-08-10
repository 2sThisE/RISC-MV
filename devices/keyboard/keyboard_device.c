#include "device_abi.h"
#include "keyboard_host.h"
#include "keyboard_protocol.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

#define KEYBOARD_CONTROL_MASK (VM_KEYBOARD_CONTROL_ENABLE | \
                               VM_KEYBOARD_CONTROL_IRQ_ENABLE)

typedef struct {
    VmDeviceHostApi host;
    VmDeviceResources resources;
    VmKeyboardHost *frontend;
    uint64_t events[VM_KEYBOARD_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint64_t control;
    uint64_t status;
    uint64_t irq_status;
    uint64_t dropped;
    atomic_flag lock;
} KeyboardDevice;

static void keyboard_lock(KeyboardDevice *keyboard)
{
    while (atomic_flag_test_and_set_explicit(&keyboard->lock,
                                             memory_order_acquire)) {
    }
}

static void keyboard_unlock(KeyboardDevice *keyboard)
{
    atomic_flag_clear_explicit(&keyboard->lock, memory_order_release);
}

static void keyboard_raise_irq(KeyboardDevice *keyboard)
{
    if (keyboard->host.raise_irq != NULL) {
        (void)keyboard->host.raise_irq(keyboard->host.context,
                                      keyboard->resources.irqs[0]);
    }
}

static void keyboard_receive(void *context, uint64_t event)
{
    KeyboardDevice *keyboard = context;
    if (keyboard == NULL) {
        return;
    }

    int raise = 0;
    keyboard_lock(keyboard);
    if ((keyboard->control & VM_KEYBOARD_CONTROL_ENABLE) != 0) {
        if (keyboard->count == VM_KEYBOARD_QUEUE_CAPACITY) {
            ++keyboard->dropped;
            keyboard->status |= VM_KEYBOARD_STATUS_OVERFLOW;
            keyboard->irq_status |= VM_KEYBOARD_IRQ_OVERFLOW;
        } else {
            keyboard->events[keyboard->tail] = event;
            keyboard->tail =
                (keyboard->tail + 1U) % VM_KEYBOARD_QUEUE_CAPACITY;
            ++keyboard->count;
            keyboard->irq_status |= VM_KEYBOARD_IRQ_EVENT;
        }
        raise = (keyboard->control &
                 VM_KEYBOARD_CONTROL_IRQ_ENABLE) != 0;
    }
    keyboard_unlock(keyboard);

    if (raise) {
        keyboard_raise_irq(keyboard);
    }
}

static int keyboard_create(const VmDeviceHostApi *host,
                           const VmDeviceResources *resources,
                           const char *configuration,
                           void **device_context)
{
    (void)configuration;
    if (host == NULL || resources == NULL || device_context == NULL ||
        host->abi_version != VM_DEVICE_ABI_VERSION ||
        host->struct_size < sizeof(*host) || host->get_service == NULL ||
        resources->struct_size < sizeof(*resources) ||
        resources->bar_count != 1 || resources->irq_count != 1) {
        return 0;
    }

    VmKeyboardHost *frontend = host->get_service(
        host->context,
        VM_KEYBOARD_SERVICE_NAME,
        VM_KEYBOARD_HOST_VERSION);
    if (frontend == NULL ||
        frontend->version != VM_KEYBOARD_HOST_VERSION ||
        frontend->struct_size < sizeof(*frontend) ||
        frontend->set_sink == NULL || frontend->clear_sink == NULL) {
        return 0;
    }

    KeyboardDevice *keyboard = calloc(1, sizeof(*keyboard));
    if (keyboard == NULL) {
        return 0;
    }
    keyboard->host = *host;
    keyboard->resources = *resources;
    keyboard->frontend = frontend;
    atomic_flag_clear(&keyboard->lock);
    if (!frontend->set_sink(frontend->context,
                            keyboard_receive,
                            keyboard)) {
        free(keyboard);
        return 0;
    }

    *device_context = keyboard;
    if (keyboard->host.log != NULL) {
        keyboard->host.log(keyboard->host.context,
                           1,
                           "keyboard device attached");
    }
    return 1;
}

static void keyboard_destroy(void *device_context)
{
    KeyboardDevice *keyboard = device_context;
    if (keyboard == NULL) {
        return;
    }
    (void)keyboard->frontend->clear_sink(keyboard->frontend->context,
                                         keyboard_receive,
                                         keyboard);
    free(keyboard);
}

static int keyboard_read(void *device_context,
                         uint32_t bar,
                         uint64_t offset,
                         uint32_t width,
                         uint64_t *value)
{
    KeyboardDevice *keyboard = device_context;
    if (keyboard == NULL || value == NULL || bar != 0 || width != 8) {
        return 0;
    }

    keyboard_lock(keyboard);
    switch (offset) {
        case VM_KEYBOARD_CONTROL_OFFSET:
            *value = keyboard->control;
            break;
        case VM_KEYBOARD_STATUS_OFFSET:
            *value = keyboard->status |
                (keyboard->count != 0
                     ? VM_KEYBOARD_STATUS_EVENT_AVAILABLE
                     : 0);
            break;
        case VM_KEYBOARD_EVENT_COUNT_OFFSET:
            *value = keyboard->count;
            break;
        case VM_KEYBOARD_EVENT_DATA_OFFSET:
            *value = 0;
            if (keyboard->count != 0) {
                *value = keyboard->events[keyboard->head];
                keyboard->head =
                    (keyboard->head + 1U) % VM_KEYBOARD_QUEUE_CAPACITY;
                --keyboard->count;
                if (keyboard->count == 0) {
                    keyboard->irq_status &= ~VM_KEYBOARD_IRQ_EVENT;
                }
            }
            break;
        case VM_KEYBOARD_CAPACITY_OFFSET:
            *value = VM_KEYBOARD_QUEUE_CAPACITY;
            break;
        case VM_KEYBOARD_IRQ_STATUS_OFFSET:
            *value = keyboard->irq_status;
            break;
        case VM_KEYBOARD_DROPPED_OFFSET:
            *value = keyboard->dropped;
            break;
        case VM_KEYBOARD_VERSION_OFFSET:
            *value = VM_KEYBOARD_PROTOCOL_VERSION;
            break;
        default:
            keyboard_unlock(keyboard);
            return 0;
    }
    keyboard_unlock(keyboard);
    return 1;
}

static int keyboard_write(void *device_context,
                          uint32_t bar,
                          uint64_t offset,
                          uint32_t width,
                          uint64_t value)
{
    KeyboardDevice *keyboard = device_context;
    if (keyboard == NULL || bar != 0 || width != 8) {
        return 0;
    }

    int raise = 0;
    keyboard_lock(keyboard);
    switch (offset) {
        case VM_KEYBOARD_CONTROL_OFFSET:
            keyboard->control = value & KEYBOARD_CONTROL_MASK;
            if ((keyboard->control & VM_KEYBOARD_CONTROL_ENABLE) == 0) {
                keyboard->head = 0;
                keyboard->tail = 0;
                keyboard->count = 0;
                keyboard->status = 0;
                keyboard->irq_status = 0;
            } else if ((keyboard->control &
                        VM_KEYBOARD_CONTROL_IRQ_ENABLE) != 0 &&
                       keyboard->irq_status != 0) {
                raise = 1;
            }
            break;
        case VM_KEYBOARD_STATUS_OFFSET:
            keyboard->status &= ~(value & VM_KEYBOARD_STATUS_OVERFLOW);
            break;
        case VM_KEYBOARD_IRQ_STATUS_OFFSET:
            keyboard->irq_status &= ~(value &
                (VM_KEYBOARD_IRQ_EVENT | VM_KEYBOARD_IRQ_OVERFLOW));
            break;
        case VM_KEYBOARD_DROPPED_OFFSET:
            if (value != 0) {
                keyboard->dropped = 0;
            }
            break;
        default:
            keyboard_unlock(keyboard);
            return 0;
    }
    keyboard_unlock(keyboard);

    if (raise) {
        keyboard_raise_irq(keyboard);
    }
    return 1;
}

static void keyboard_reset(void *device_context)
{
    KeyboardDevice *keyboard = device_context;
    if (keyboard == NULL) {
        return;
    }
    keyboard_lock(keyboard);
    keyboard->head = 0;
    keyboard->tail = 0;
    keyboard->count = 0;
    keyboard->control = 0;
    keyboard->status = 0;
    keyboard->irq_status = 0;
    keyboard->dropped = 0;
    keyboard_unlock(keyboard);
}

static const VmDeviceModule KEYBOARD_MODULE = {
    .abi_version = VM_DEVICE_ABI_VERSION,
    .struct_size = sizeof(VmDeviceModule),
    .descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .name = "hid-keyboard",
        .device_class = VM_DEVICE_CLASS_INPUT,
        .vendor_id = UINT32_C(0x564D),
        .device_id = UINT32_C(0x3000),
        .device_version = 1,
        .features = 0,
        .bar_count = 1,
        .irq_count = 1,
        .bar_sizes = { VM_KEYBOARD_MMIO_SIZE },
        .bar_alignments = { UINT64_C(4096) }
    },
    .create = keyboard_create,
    .destroy = keyboard_destroy,
    .read = keyboard_read,
    .write = keyboard_write,
    .tick = NULL,
    .reset = keyboard_reset
};

const VmDeviceModule *vm_keyboard_device_module(void)
{
    return &KEYBOARD_MODULE;
}

#ifndef VM_KEYBOARD_DEVICE_STATIC
#ifdef _WIN32
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const VmDeviceModule *vm_device_query(uint32_t host_abi_version)
{
    return host_abi_version == VM_DEVICE_ABI_VERSION
               ? &KEYBOARD_MODULE
               : NULL;
}
#endif
