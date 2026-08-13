#ifndef CORE_CONTROL_H
#define CORE_CONTROL_H

#include <stdint.h>

#include "bus.h"
#include "core_control_protocol.h"

struct VirtualMachine;

typedef struct {
    struct VirtualMachine *vm;
    uint64_t target_core;
    uint64_t target_thread;
    uint64_t entry_pc;
    uint64_t ipi_line;
    uint64_t last_command;
    uint64_t last_result;
} CoreControlDevice;

int core_control_device_init(CoreControlDevice *device,
                             struct VirtualMachine *vm);
BusDevice core_control_device_as_bus_device(CoreControlDevice *device);

#endif
