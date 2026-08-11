#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#include "bus.h"
#include "builtin_device_protocol.h"

typedef struct {
    uint64_t control;
    uint64_t counter;
    uint64_t compare;
    uint64_t status;
    unsigned int interrupt_line;
} TimerDevice;

int timer_device_init(TimerDevice *timer, unsigned int interrupt_line);
BusDevice timer_device_as_bus_device(TimerDevice *timer);

#endif
