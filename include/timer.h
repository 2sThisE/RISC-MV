#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#include "bus.h"

#define TIMER_MMIO_SIZE UINT64_C(32)
#define TIMER_TICKS_PER_SECOND UINT64_C(1000)

#define TIMER_CONTROL_OFFSET UINT64_C(0)
#define TIMER_COUNTER_OFFSET UINT64_C(8)
#define TIMER_COMPARE_OFFSET UINT64_C(16)
#define TIMER_STATUS_OFFSET  UINT64_C(24)

#define TIMER_CONTROL_ENABLE     (UINT64_C(1) << 0)
#define TIMER_CONTROL_REPEAT     (UINT64_C(1) << 1)
#define TIMER_CONTROL_IRQ_ENABLE (UINT64_C(1) << 2)
#define TIMER_STATUS_PENDING     (UINT64_C(1) << 0)

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
