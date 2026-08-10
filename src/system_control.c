#include "system_control.h"

#include <stddef.h>

#include "vm.h"

static void execute_command(SystemControlDevice *device, uint64_t command)
{
    device->last_command = command;
    switch (command) {
        case SYSTEM_CONTROL_COMMAND_NONE:
            device->last_result = SYSTEM_CONTROL_RESULT_NONE;
            return;
        case SYSTEM_CONTROL_COMMAND_SHUTDOWN:
            if (vm_request_system_action(device->vm,
                                         VM_SYSTEM_ACTION_SHUTDOWN)) {
                device->status = SYSTEM_CONTROL_STATUS_SHUTDOWN_PENDING;
                device->last_result = SYSTEM_CONTROL_RESULT_ACCEPTED;
            } else {
                device->last_result = SYSTEM_CONTROL_RESULT_BUSY;
            }
            return;
        case SYSTEM_CONTROL_COMMAND_WARM_RESET:
            if (vm_request_system_action(device->vm,
                                         VM_SYSTEM_ACTION_WARM_RESET)) {
                device->warm_reset_pending = 1;
                device->status = SYSTEM_CONTROL_STATUS_RESET_PENDING;
                device->last_result = SYSTEM_CONTROL_RESULT_ACCEPTED;
            } else {
                device->last_result = SYSTEM_CONTROL_RESULT_BUSY;
            }
            return;
        default:
            device->last_result = SYSTEM_CONTROL_RESULT_INVALID_COMMAND;
            return;
    }
}

static int system_control_read(void *context,
                               uint64_t offset,
                               size_t width,
                               uint64_t *value)
{
    const SystemControlDevice *device = context;
    if (device == NULL || value == NULL || width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case SYSTEM_CONTROL_MAGIC_OFFSET:
            *value = SYSTEM_CONTROL_MAGIC;
            return 1;
        case SYSTEM_CONTROL_VERSION_OFFSET:
            *value = SYSTEM_CONTROL_VERSION;
            return 1;
        case SYSTEM_CONTROL_FEATURES_OFFSET:
            *value = SYSTEM_CONTROL_FEATURE_SHUTDOWN |
                     SYSTEM_CONTROL_FEATURE_WARM_RESET;
            return 1;
        case SYSTEM_CONTROL_COMMAND_OFFSET:
            *value = device->last_command;
            return 1;
        case SYSTEM_CONTROL_STATUS_OFFSET:
            *value = device->status;
            return 1;
        case SYSTEM_CONTROL_RESULT_OFFSET:
            *value = device->last_result;
            return 1;
        case SYSTEM_CONTROL_RESET_CAUSE_OFFSET:
            *value = device->reset_cause;
            return 1;
        case SYSTEM_CONTROL_RESET_COUNT_OFFSET:
            *value = device->reset_count;
            return 1;
        default:
            return 0;
    }
}

static int system_control_write(void *context,
                                uint64_t offset,
                                size_t width,
                                uint64_t value)
{
    SystemControlDevice *device = context;
    if (device == NULL || width != sizeof(uint64_t) ||
        offset != SYSTEM_CONTROL_COMMAND_OFFSET) {
        return 0;
    }

    execute_command(device, value);
    return 1;
}

static void system_control_reset(void *context)
{
    SystemControlDevice *device = context;
    if (device == NULL) {
        return;
    }

    if (device->warm_reset_pending) {
        device->reset_cause = SYSTEM_CONTROL_RESET_CAUSE_SOFTWARE;
        ++device->reset_count;
    }
    device->last_command = SYSTEM_CONTROL_COMMAND_NONE;
    device->status = SYSTEM_CONTROL_STATUS_RUNNING;
    device->last_result = SYSTEM_CONTROL_RESULT_NONE;
    device->warm_reset_pending = 0;
}

int system_control_device_init(SystemControlDevice *device,
                               struct VirtualMachine *vm)
{
    if (device == NULL || vm == NULL || vm->cores == NULL) {
        return 0;
    }

    *device = (SystemControlDevice){0};
    device->vm = vm;
    device->reset_cause = SYSTEM_CONTROL_RESET_CAUSE_POWER_ON;
    return 1;
}

BusDevice system_control_device_as_bus_device(SystemControlDevice *device)
{
    BusDevice bus_device = {
        .context = device,
        .read = system_control_read,
        .write = system_control_write,
        .tick = NULL,
        .reset = system_control_reset
    };
    return bus_device;
}
