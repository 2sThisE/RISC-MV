#include "timer.h"

#include <stddef.h>

#define TIMER_CONTROL_MASK (TIMER_CONTROL_ENABLE | \
                            TIMER_CONTROL_REPEAT | \
                            TIMER_CONTROL_IRQ_ENABLE)

static int timer_read(void *context,
                      uint64_t offset,
                      size_t width,
                      uint64_t *value)
{
    TimerDevice *timer = context;

    if (timer == NULL || value == NULL || width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case TIMER_CONTROL_OFFSET:
            *value = timer->control;
            return 1;
        case TIMER_COUNTER_OFFSET:
            *value = timer->counter;
            return 1;
        case TIMER_COMPARE_OFFSET:
            *value = timer->compare;
            return 1;
        case TIMER_STATUS_OFFSET:
            *value = timer->status;
            return 1;
        default:
            return 0;
    }
}

static int timer_write(void *context,
                       uint64_t offset,
                       size_t width,
                       uint64_t value)
{
    TimerDevice *timer = context;

    if (timer == NULL || width != sizeof(uint64_t)) {
        return 0;
    }

    switch (offset) {
        case TIMER_CONTROL_OFFSET:
            timer->control = value & TIMER_CONTROL_MASK;
            return 1;
        case TIMER_COUNTER_OFFSET:
            timer->counter = value;
            return 1;
        case TIMER_COMPARE_OFFSET:
            timer->compare = value;
            return 1;
        case TIMER_STATUS_OFFSET:
            if ((value & TIMER_STATUS_PENDING) != 0) {
                timer->status &= ~TIMER_STATUS_PENDING;
            }
            return 1;
        default:
            return 0;
    }
}

static void timer_tick(void *context,
                       uint64_t ticks,
                       InterruptRouter *interrupts)
{
    TimerDevice *timer = context;

    if (timer == NULL || ticks == 0 ||
        (timer->control & TIMER_CONTROL_ENABLE) == 0) {
        return;
    }

    if (timer->compare == 0) {
        timer->counter += ticks;
        return;
    }

    uint64_t until_compare = timer->counter < timer->compare
        ? timer->compare - timer->counter
        : 0;

    if (until_compare != 0 && ticks < until_compare) {
        timer->counter += ticks;
        return;
    }

    uint64_t remaining = until_compare == 0 ? ticks : ticks - until_compare;
    timer->status |= TIMER_STATUS_PENDING;

    if ((timer->control & TIMER_CONTROL_IRQ_ENABLE) != 0) {
        (void)interrupt_router_raise(interrupts,
                                     timer->interrupt_line);
    }

    if ((timer->control & TIMER_CONTROL_REPEAT) != 0) {
        timer->counter = remaining % timer->compare;
    } else {
        timer->counter = timer->compare;
        timer->control &= ~TIMER_CONTROL_ENABLE;
    }
}

static void timer_reset(void *context)
{
    TimerDevice *timer = context;
    if (timer == NULL) {
        return;
    }
    unsigned int interrupt_line = timer->interrupt_line;
    *timer = (TimerDevice){0};
    timer->interrupt_line = interrupt_line;
}

int timer_device_init(TimerDevice *timer, unsigned int interrupt_line)
{
    if (timer == NULL || interrupt_line >= INTERRUPT_LINE_COUNT) {
        return 0;
    }

    *timer = (TimerDevice){0};
    timer->interrupt_line = interrupt_line;
    return 1;
}

BusDevice timer_device_as_bus_device(TimerDevice *timer)
{
    BusDevice device = {
        .context = timer,
        .read = timer_read,
        .write = timer_write,
        .tick = timer_tick,
        .reset = timer_reset
    };

    return device;
}
