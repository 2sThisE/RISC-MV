#ifndef IRQ_CONTROLLER_H
#define IRQ_CONTROLLER_H

#include <stdint.h>

#include "bus.h"
#include "interrupt.h"
#include "builtin_device_protocol.h"

typedef struct {
    InterruptRouter *router;
    uint64_t selected_line;
    uint64_t last_result;
} IrqControllerDevice;

int irq_controller_device_init(IrqControllerDevice *device,
                               InterruptRouter *router);
BusDevice irq_controller_device_as_bus_device(IrqControllerDevice *device);

#endif
