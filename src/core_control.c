#include "core_control.h"

#include <stddef.h>
#include <stdatomic.h>

#include "vm.h"

static HardwareThread *selected_thread(CoreControlDevice *device)
{
    if (device->target_core > SIZE_MAX ||
        device->target_thread > SIZE_MAX) {
        return NULL;
    }

    return vm_hardware_thread(device->vm,
                              (size_t)device->target_core,
                              (size_t)device->target_thread);
}

static uint64_t selected_status(CoreControlDevice *device)
{
    HardwareThread *thread = selected_thread(device);
    if (thread == NULL) {
        return CORE_CONTROL_STATUS_INVALID;
    }

    switch (atomic_load_explicit(&thread->state,
                                 memory_order_acquire)) {
        case HARDWARE_THREAD_OFFLINE:
            return CORE_CONTROL_STATUS_OFFLINE;
        case HARDWARE_THREAD_STARTING:
            return CORE_CONTROL_STATUS_STARTING;
        case HARDWARE_THREAD_RUNNABLE:
            return CORE_CONTROL_STATUS_RUNNABLE;
        case HARDWARE_THREAD_HALTED:
            return CORE_CONTROL_STATUS_HALTED;
        case HARDWARE_THREAD_WAITING:
            return CORE_CONTROL_STATUS_WAITING;
        default:
            return CORE_CONTROL_STATUS_INVALID;
    }
}

static void execute_command(CoreControlDevice *device, uint64_t command)
{
    HardwareThread *thread = selected_thread(device);
    device->last_command = command;

    if (command == CORE_CONTROL_COMMAND_NONE) {
        device->last_result = CORE_CONTROL_RESULT_NONE;
        return;
    }
    if (thread == NULL) {
        device->last_result = CORE_CONTROL_RESULT_INVALID_TARGET;
        return;
    }

    size_t core_id = (size_t)device->target_core;
    size_t thread_id = (size_t)device->target_thread;
    switch (command) {
        case CORE_CONTROL_COMMAND_START: {
            uint64_t first_instruction_byte;
            if (!bus_fetch(device->vm->bus,
                           device->entry_pc,
                           1,
                           &first_instruction_byte)) {
                device->last_result = CORE_CONTROL_RESULT_INVALID_ENTRY;
            } else if (vm_activate_hardware_thread(device->vm,
                                                   core_id,
                                                   thread_id,
                                                   device->entry_pc)) {
                device->last_result = CORE_CONTROL_RESULT_SUCCESS;
            } else {
                device->last_result = CORE_CONTROL_RESULT_INVALID_STATE;
            }
            break;
        }
        case CORE_CONTROL_COMMAND_STOP:
            device->last_result = vm_request_hardware_thread_stop(
                device->vm,
                core_id,
                thread_id)
                ? CORE_CONTROL_RESULT_SUCCESS
                : CORE_CONTROL_RESULT_INVALID_STATE;
            break;
        case CORE_CONTROL_COMMAND_RESET:
            device->last_result = vm_reset_hardware_thread(device->vm,
                                                            core_id,
                                                            thread_id)
                ? CORE_CONTROL_RESULT_SUCCESS
                : CORE_CONTROL_RESULT_INVALID_STATE;
            break;
        case CORE_CONTROL_COMMAND_IPI:
            if (device->ipi_line < INTERRUPT_IPI_LINE_BASE ||
                device->ipi_line >= INTERRUPT_LINE_COUNT) {
                device->last_result =
                    CORE_CONTROL_RESULT_INVALID_INTERRUPT;
            } else {
                device->last_result =
                    vm_raise_hardware_thread_interrupt(
                        device->vm,
                        core_id,
                        thread_id,
                        (unsigned int)device->ipi_line)
                    ? CORE_CONTROL_RESULT_SUCCESS
                    : CORE_CONTROL_RESULT_INVALID_STATE;
            }
            break;
        default:
            device->last_result = CORE_CONTROL_RESULT_INVALID_COMMAND;
            break;
    }
}

static int core_control_read(void *context,
                             uint64_t offset,
                             size_t width,
                             uint64_t *value)
{
    CoreControlDevice *device = context;
    if (device == NULL || value == NULL || width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case CORE_CONTROL_TARGET_CORE_OFFSET:
            *value = device->target_core;
            return 1;
        case CORE_CONTROL_TARGET_THREAD_OFFSET:
            *value = device->target_thread;
            return 1;
        case CORE_CONTROL_ENTRY_PC_OFFSET:
            *value = device->entry_pc;
            return 1;
        case CORE_CONTROL_COMMAND_OFFSET:
            *value = device->last_command;
            return 1;
        case CORE_CONTROL_STATUS_OFFSET:
            *value = selected_status(device);
            return 1;
        case CORE_CONTROL_RESULT_OFFSET:
            *value = device->last_result;
            return 1;
        case CORE_CONTROL_IPI_LINE_OFFSET:
            *value = device->ipi_line;
            return 1;
        default:
            return 0;
    }
}

static int core_control_write(void *context,
                              uint64_t offset,
                              size_t width,
                              uint64_t value)
{
    CoreControlDevice *device = context;
    if (device == NULL || width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case CORE_CONTROL_TARGET_CORE_OFFSET:
            device->target_core = value;
            return 1;
        case CORE_CONTROL_TARGET_THREAD_OFFSET:
            device->target_thread = value;
            return 1;
        case CORE_CONTROL_ENTRY_PC_OFFSET:
            device->entry_pc = value;
            return 1;
        case CORE_CONTROL_IPI_LINE_OFFSET:
            device->ipi_line = value;
            return 1;
        case CORE_CONTROL_COMMAND_OFFSET:
            execute_command(device, value);
            return 1;
        default:
            return 0;
    }
}

int core_control_device_init(CoreControlDevice *device,
                             struct VirtualMachine *vm)
{
    if (device == NULL || vm == NULL || vm->cores == NULL) {
        return 0;
    }

    *device = (CoreControlDevice){0};
    device->vm = vm;
    return 1;
}

static void core_control_reset(void *context)
{
    CoreControlDevice *device = context;
    if (device == NULL) {
        return;
    }
    struct VirtualMachine *vm = device->vm;
    *device = (CoreControlDevice){0};
    device->vm = vm;
}

BusDevice core_control_device_as_bus_device(CoreControlDevice *device)
{
    BusDevice bus_device = {
        .context = device,
        .read = core_control_read,
        .write = core_control_write,
        .tick = NULL,
        .reset = core_control_reset
    };
    return bus_device;
}
