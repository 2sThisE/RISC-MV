#include "vm_clock.h"

#include <stddef.h>

uint64_t clock_source_poll(ClockSource *source)
{
    if (source != NULL && source->poll != NULL) {
        return source->poll(source->context);
    }

    return 0;
}

static uint64_t instruction_clock_poll(void *context)
{
    InstructionClock *clock = context;

    if (clock == NULL) {
        return 0;
    }

    return clock->ticks_per_instruction;
}

int instruction_clock_init(InstructionClock *clock,
                           ClockSource *source,
                           uint64_t ticks_per_instruction)
{
    if (clock == NULL || source == NULL ||
        ticks_per_instruction == 0) {
        return 0;
    }

    clock->ticks_per_instruction = ticks_per_instruction;

    source->poll = instruction_clock_poll;
    source->context = clock;
    return 1;
}
