#include "irq_controller.h"

#include <stddef.h>

static int irq_controller_read(void *context,
                               uint64_t offset,
                               size_t width,
                               uint64_t *value)
{
    IrqControllerDevice *device = context;
    if (device == NULL || device->router == NULL || value == NULL ||
        width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case IRQ_CONTROLLER_MAGIC_OFFSET:
            *value = IRQ_CONTROLLER_MAGIC;
            return 1;
        case IRQ_CONTROLLER_VERSION_OFFSET:
            *value = IRQ_CONTROLLER_VERSION;
            return 1;
        case IRQ_CONTROLLER_LINE_COUNT_OFFSET:
            *value = INTERRUPT_EXTERNAL_LINE_COUNT;
            return 1;
        case IRQ_CONTROLLER_TARGET_COUNT_OFFSET:
            *value = (uint64_t)device->router->target_count;
            return 1;
        case IRQ_CONTROLLER_ENABLE_OFFSET:
            *value = interrupt_router_enabled(device->router);
            return 1;
        case IRQ_CONTROLLER_PENDING_OFFSET:
            *value = interrupt_router_pending(device->router);
            return 1;
        case IRQ_CONTROLLER_ACTIVE_OFFSET:
            *value = interrupt_router_active(device->router);
            return 1;
        case IRQ_CONTROLLER_LINE_SELECT_OFFSET:
            *value = device->selected_line;
            return 1;
        case IRQ_CONTROLLER_ROUTE_OFFSET: {
            size_t target;
            if (device->selected_line >= INTERRUPT_EXTERNAL_LINE_COUNT ||
                !interrupt_router_get_route(
                    device->router,
                    (unsigned int)device->selected_line,
                    &target)) {
                return 0;
            }
            *value = (uint64_t)target;
            return 1;
        }
        case IRQ_CONTROLLER_RESULT_OFFSET:
            *value = device->last_result;
            return 1;
        case IRQ_CONTROLLER_FEATURES_OFFSET:
            *value = IRQ_CONTROLLER_FEATURE_MASKING |
                     IRQ_CONTROLLER_FEATURE_ROUTING |
                     IRQ_CONTROLLER_FEATURE_EOI |
                     IRQ_CONTROLLER_FEATURE_FIXED_PRIORITY |
                     IRQ_CONTROLLER_FEATURE_NO_REENTRY;
            return 1;
        default:
            return 0;
    }
}

static int mask_valid(uint64_t mask)
{
    return (mask & ~INTERRUPT_EXTERNAL_LINE_MASK) == 0;
}

static int irq_controller_write(void *context,
                                uint64_t offset,
                                size_t width,
                                uint64_t value)
{
    IrqControllerDevice *device = context;
    if (device == NULL || device->router == NULL ||
        width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case IRQ_CONTROLLER_ENABLE_OFFSET:
            if (!mask_valid(value)) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_MASK;
            } else if (interrupt_router_set_enabled(device->router,
                                                    value)) {
                device->last_result = IRQ_CONTROLLER_RESULT_SUCCESS;
            } else {
                device->last_result = IRQ_CONTROLLER_RESULT_BUSY;
            }
            return 1;
        case IRQ_CONTROLLER_ENABLE_SET_OFFSET:
            if (!mask_valid(value)) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_MASK;
            } else if (interrupt_router_enable_lines(device->router,
                                                     value)) {
                device->last_result = IRQ_CONTROLLER_RESULT_SUCCESS;
            } else {
                device->last_result = IRQ_CONTROLLER_RESULT_BUSY;
            }
            return 1;
        case IRQ_CONTROLLER_ENABLE_CLEAR_OFFSET:
            if (!mask_valid(value)) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_MASK;
            } else if (interrupt_router_disable_lines(device->router,
                                                      value)) {
                device->last_result = IRQ_CONTROLLER_RESULT_SUCCESS;
            } else {
                device->last_result = IRQ_CONTROLLER_RESULT_BUSY;
            }
            return 1;
        case IRQ_CONTROLLER_LINE_SELECT_OFFSET:
            if (value >= INTERRUPT_EXTERNAL_LINE_COUNT) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_LINE;
            } else {
                device->selected_line = value;
                device->last_result = IRQ_CONTROLLER_RESULT_SUCCESS;
            }
            return 1;
        case IRQ_CONTROLLER_ROUTE_OFFSET:
            if (device->selected_line >=
                INTERRUPT_EXTERNAL_LINE_COUNT) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_LINE;
            } else if (value >= device->router->target_count) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_TARGET;
            } else if (!interrupt_router_set_route(
                           device->router,
                           (unsigned int)device->selected_line,
                           (size_t)value)) {
                device->last_result = IRQ_CONTROLLER_RESULT_BUSY;
            } else {
                device->last_result = IRQ_CONTROLLER_RESULT_SUCCESS;
            }
            return 1;
        case IRQ_CONTROLLER_EOI_OFFSET:
            if (value >= INTERRUPT_EXTERNAL_LINE_COUNT) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_INVALID_LINE;
            } else if (!interrupt_router_eoi(device->router,
                                             (unsigned int)value)) {
                device->last_result =
                    IRQ_CONTROLLER_RESULT_NOT_ACTIVE;
            } else {
                device->last_result = IRQ_CONTROLLER_RESULT_SUCCESS;
            }
            return 1;
        default:
            return 0;
    }
}

static void irq_controller_reset(void *context)
{
    IrqControllerDevice *device = context;
    if (device == NULL || device->router == NULL) {
        return;
    }
    interrupt_router_programmable_reset(device->router);
    device->selected_line = 0;
    device->last_result = IRQ_CONTROLLER_RESULT_NONE;
}

int irq_controller_device_init(IrqControllerDevice *device,
                               InterruptRouter *router)
{
    if (device == NULL || router == NULL || router->targets == NULL ||
        router->target_count == 0) {
        return 0;
    }

    *device = (IrqControllerDevice){0};
    device->router = router;
    interrupt_router_programmable_reset(router);
    return 1;
}

BusDevice irq_controller_device_as_bus_device(IrqControllerDevice *device)
{
    BusDevice bus_device = {
        .context = device,
        .read = irq_controller_read,
        .write = irq_controller_write,
        .tick = NULL,
        .reset = irq_controller_reset
    };
    return bus_device;
}
