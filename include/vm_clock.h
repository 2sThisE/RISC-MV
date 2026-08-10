#ifndef VM_CLOCK_H
#define VM_CLOCK_H

#include <stdint.h>

typedef uint64_t (*ClockPollFunction)(void *context);

typedef struct {
    ClockPollFunction poll;
    void *context;
} ClockSource;

typedef struct {
    uint64_t ticks_per_instruction;
} InstructionClock;

uint64_t clock_source_poll(ClockSource *source);
int instruction_clock_init(InstructionClock *clock,
                           ClockSource *source,
                           uint64_t ticks_per_instruction);

#endif
