#ifndef VM_H
#define VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "bus.h"
#include "cpu.h"
#include "host_thread.h"
#include "interrupt.h"
#include "ram.h"

#define VM_MAX_CORES 64U
#define VM_MAX_THREADS_PER_CORE 8U

typedef enum {
    HARDWARE_THREAD_OFFLINE = 0,
    HARDWARE_THREAD_STARTING,
    HARDWARE_THREAD_RUNNABLE,
    HARDWARE_THREAD_HALTED,
    HARDWARE_THREAD_WAITING
} HardwareThreadState;

typedef enum {
    VM_SYSTEM_ACTION_NONE = 0,
    VM_SYSTEM_ACTION_SHUTDOWN,
    VM_SYSTEM_ACTION_WARM_RESET
} VmSystemAction;

typedef struct {
    CPU cpu;
    InterruptController interrupts;
    atomic_int state;
    atomic_bool stop_requested;
} HardwareThread;

struct VirtualMachine;

typedef struct {
    HardwareThread *threads;
    size_t thread_count;
    size_t next_thread;
    size_t id;
    HostThread worker;
    struct VirtualMachine *vm;
} Core;

typedef struct VirtualMachine {
    RAM *ram;
    Bus *bus;
    Core *cores;
    size_t core_count;
    size_t threads_per_core;
    size_t logical_processor_count;
    InterruptController **interrupt_targets;
    InterruptRouter interrupt_router;
    HostThread device_worker;
    atomic_size_t active_thread_count;
    atomic_bool stop_requested;
    atomic_bool failed;
    atomic_int system_action;
    uint64_t reset_vector;
} VirtualMachine;

int vm_init(VirtualMachine *vm,
            RAM *ram,
            Bus *bus,
            size_t core_count,
            size_t threads_per_core);
void vm_destroy(VirtualMachine *vm);
int vm_run(VirtualMachine *vm);
int vm_set_reset_vector(VirtualMachine *vm, uint64_t reset_vector);
int vm_request_system_action(VirtualMachine *vm, VmSystemAction action);
VmSystemAction vm_system_action(const VirtualMachine *vm);
int vm_activate_hardware_thread(VirtualMachine *vm,
                                size_t core_id,
                                size_t thread_id,
                                uint64_t entry_address);
int vm_request_hardware_thread_stop(VirtualMachine *vm,
                                    size_t core_id,
                                    size_t thread_id);
int vm_reset_hardware_thread(VirtualMachine *vm,
                             size_t core_id,
                             size_t thread_id);
HardwareThread *vm_hardware_thread(VirtualMachine *vm,
                                   size_t core_id,
                                   size_t thread_id);
int vm_set_vector_base(VirtualMachine *vm,
                       size_t logical_processor,
                       uint64_t address);
int vm_raise_hardware_thread_interrupt(VirtualMachine *vm,
                                       size_t core_id,
                                       size_t thread_id,
                                       unsigned int line);
int vm_route_interrupt(VirtualMachine *vm,
                       unsigned int line,
                       size_t logical_processor);

#endif
