#ifndef CORE_CONTROL_H
#define CORE_CONTROL_H

#include <stdint.h>

#include "bus.h"

#define CORE_CONTROL_MMIO_BASE UINT64_C(0xFFFFFFFFFFFE0000)
#define CORE_CONTROL_MMIO_SIZE UINT64_C(56)

#define CORE_CONTROL_TARGET_CORE_OFFSET   UINT64_C(0)
#define CORE_CONTROL_TARGET_THREAD_OFFSET UINT64_C(8)
#define CORE_CONTROL_ENTRY_PC_OFFSET      UINT64_C(16)
#define CORE_CONTROL_COMMAND_OFFSET       UINT64_C(24)
#define CORE_CONTROL_STATUS_OFFSET        UINT64_C(32)
#define CORE_CONTROL_RESULT_OFFSET        UINT64_C(40)
#define CORE_CONTROL_IPI_LINE_OFFSET      UINT64_C(48)

#define CORE_CONTROL_COMMAND_NONE  UINT64_C(0)
#define CORE_CONTROL_COMMAND_START UINT64_C(1)
#define CORE_CONTROL_COMMAND_STOP  UINT64_C(2)
#define CORE_CONTROL_COMMAND_RESET UINT64_C(3)
#define CORE_CONTROL_COMMAND_IPI   UINT64_C(4)

#define CORE_CONTROL_STATUS_OFFLINE  UINT64_C(0)
#define CORE_CONTROL_STATUS_STARTING UINT64_C(1)
#define CORE_CONTROL_STATUS_RUNNABLE UINT64_C(2)
#define CORE_CONTROL_STATUS_HALTED   UINT64_C(3)
#define CORE_CONTROL_STATUS_WAITING  UINT64_C(4)
#define CORE_CONTROL_STATUS_INVALID  UINT64_MAX

#define CORE_CONTROL_RESULT_NONE            UINT64_C(0)
#define CORE_CONTROL_RESULT_SUCCESS         UINT64_C(1)
#define CORE_CONTROL_RESULT_INVALID_TARGET  UINT64_C(2)
#define CORE_CONTROL_RESULT_INVALID_ENTRY   UINT64_C(3)
#define CORE_CONTROL_RESULT_INVALID_STATE   UINT64_C(4)
#define CORE_CONTROL_RESULT_INVALID_COMMAND UINT64_C(5)
#define CORE_CONTROL_RESULT_INVALID_INTERRUPT UINT64_C(6)

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
