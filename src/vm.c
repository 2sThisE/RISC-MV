#include "vm.h"

#include <limits.h>
#include <stdlib.h>

#define VM_DEVICE_TIMER_POLL_NANOSECONDS UINT64_C(100000)

static HardwareThread *select_hardware_thread(Core *core)
{
    for (size_t checked = 0; checked < core->thread_count; ++checked) {
        size_t index = core->next_thread;
        core->next_thread = (core->next_thread + 1) % core->thread_count;

        if (atomic_load_explicit(&core->threads[index].state,
                                 memory_order_acquire) ==
            HARDWARE_THREAD_RUNNABLE) {
            return &core->threads[index];
        }
    }
    return NULL;
}

static void finish_hardware_thread(VirtualMachine *vm,
                                   HardwareThread *thread)
{
    int state = atomic_load_explicit(&thread->state,
                                     memory_order_acquire);
    for (;;) {
        if (state != HARDWARE_THREAD_RUNNABLE &&
            state != HARDWARE_THREAD_WAITING) {
            return;
        }
        if (atomic_compare_exchange_weak_explicit(
                &thread->state,
                &state,
                HARDWARE_THREAD_HALTED,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }

    thread->cpu.waiting = 0;
    thread->cpu.halted = 1;
    atomic_store_explicit(&thread->stop_requested,
                          0,
                          memory_order_release);
    if (atomic_fetch_sub_explicit(&vm->active_thread_count,
                                  1,
                                  memory_order_acq_rel) == 1) {
        atomic_store_explicit(&vm->stop_requested,
                              1,
                              memory_order_release);
    }
}

static int service_waiting_hardware_threads(Core *core)
{
    VirtualMachine *vm = core->vm;
    int serviced = 0;

    for (size_t index = 0; index < core->thread_count; ++index) {
        HardwareThread *thread = &core->threads[index];
        if (atomic_load_explicit(&thread->state,
                                 memory_order_acquire) !=
            HARDWARE_THREAD_WAITING) {
            continue;
        }

        if (atomic_exchange_explicit(&thread->stop_requested,
                                     0,
                                     memory_order_acq_rel)) {
            finish_hardware_thread(vm, thread);
            serviced = 1;
            continue;
        }

        if (!cpu_check_interrupt(&thread->cpu,
                                 vm->bus,
                                 &thread->interrupts)) {
            return -1;
        }

        if (!thread->cpu.waiting) {
            int expected_state = HARDWARE_THREAD_WAITING;
            if (atomic_compare_exchange_strong_explicit(
                    &thread->state,
                    &expected_state,
                    HARDWARE_THREAD_RUNNABLE,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                serviced = 1;
            }
        }
    }

    return serviced;
}

static int core_worker(void *context)
{
    Core *core = context;
    VirtualMachine *vm = core->vm;

    while (!atomic_load_explicit(&vm->stop_requested,
                                 memory_order_acquire)) {
        int waiting_result = service_waiting_hardware_threads(core);
        if (waiting_result < 0) {
            atomic_store_explicit(&vm->failed, 1, memory_order_release);
            atomic_store_explicit(&vm->stop_requested,
                                  1,
                                  memory_order_release);
            return 0;
        }

        HardwareThread *thread = select_hardware_thread(core);
        if (thread == NULL) {
            if (waiting_result == 0) {
                host_thread_sleep_milliseconds(1);
            }
            continue;
        }

        if (atomic_exchange_explicit(&thread->stop_requested,
                                     0,
                                     memory_order_acq_rel)) {
            finish_hardware_thread(vm, thread);
            continue;
        }

        if (!cpu_step(&thread->cpu, vm->bus) ||
            (!thread->cpu.halted &&
             !cpu_check_interrupt(&thread->cpu,
                                  vm->bus,
                                  &thread->interrupts))) {
            atomic_store_explicit(&vm->failed, 1, memory_order_release);
            atomic_store_explicit(&vm->stop_requested,
                                  1,
                                  memory_order_release);
            return 0;
        }

        if (thread->cpu.halted ||
            atomic_exchange_explicit(&thread->stop_requested,
                                     0,
                                     memory_order_acq_rel)) {
            finish_hardware_thread(vm, thread);
        } else if (thread->cpu.waiting) {
            int expected_state = HARDWARE_THREAD_RUNNABLE;
            (void)atomic_compare_exchange_strong_explicit(
                &thread->state,
                &expected_state,
                HARDWARE_THREAD_WAITING,
                memory_order_acq_rel,
                memory_order_acquire);
        }
    }

    return !atomic_load_explicit(&vm->failed, memory_order_acquire);
}

static int device_worker(void *context)
{
    VirtualMachine *vm = context;
    HostHighResolutionTimer poll_timer;
    if (!host_high_resolution_timer_init(&poll_timer)) {
        atomic_store_explicit(&vm->failed, 1, memory_order_release);
        atomic_store_explicit(&vm->stop_requested, 1, memory_order_release);
        return 0;
    }
    uint64_t previous = host_monotonic_nanoseconds();

    while (!atomic_load_explicit(&vm->stop_requested,
                                 memory_order_acquire)) {
        uint64_t now = host_monotonic_nanoseconds();
        if (now > previous) {
            bus_tick(vm->bus,
                     now - previous,
                     &vm->interrupt_router);
            previous = now;
        }

        if (!host_high_resolution_timer_wait(
                &poll_timer, VM_DEVICE_TIMER_POLL_NANOSECONDS)) {
            host_high_resolution_timer_destroy(&poll_timer);
            atomic_store_explicit(&vm->failed, 1, memory_order_release);
            atomic_store_explicit(&vm->stop_requested,
                                  1,
                                  memory_order_release);
            return 0;
        }
    }

    host_high_resolution_timer_destroy(&poll_timer);
    return 1;
}

static void release_core_storage(VirtualMachine *vm)
{
    if (vm->cores != NULL) {
        for (size_t i = 0; i < vm->core_count; ++i) {
            free(vm->cores[i].threads);
        }
    }
    free(vm->interrupt_targets);
    free(vm->cores);
    vm->interrupt_targets = NULL;
    vm->cores = NULL;
}

int vm_init(VirtualMachine *vm,
            RAM *ram,
            Bus *bus,
            size_t core_count,
            size_t threads_per_core)
{
    if (vm == NULL || ram == NULL || bus == NULL || bus->ram != ram ||
        core_count == 0 || core_count > VM_MAX_CORES ||
        threads_per_core == 0 ||
        threads_per_core > VM_MAX_THREADS_PER_CORE ||
        core_count > SIZE_MAX / threads_per_core) {
        return 0;
    }

    size_t logical_processors = core_count * threads_per_core;

    *vm = (VirtualMachine){0};
    vm->ram = ram;
    vm->bus = bus;
    vm->core_count = core_count;
    vm->threads_per_core = threads_per_core;
    vm->logical_processor_count = logical_processors;
    atomic_init(&vm->active_thread_count, 1);
    atomic_init(&vm->stop_requested, 0);
    atomic_init(&vm->failed, 0);
    atomic_init(&vm->system_action, VM_SYSTEM_ACTION_NONE);
    vm->reset_vector = 0;

    vm->cores = malloc(core_count * sizeof(*vm->cores));
    vm->interrupt_targets =
        malloc(logical_processors * sizeof(*vm->interrupt_targets));
    if (vm->cores == NULL || vm->interrupt_targets == NULL) {
        free(vm->interrupt_targets);
        free(vm->cores);
        vm->interrupt_targets = NULL;
        vm->cores = NULL;
        return 0;
    }

    for (size_t i = 0; i < core_count; ++i) {
        vm->cores[i] = (Core){0};
    }

    for (size_t i = 0; i < core_count; ++i) {
        vm->cores[i].id = i;
        vm->cores[i].thread_count = threads_per_core;
        vm->cores[i].vm = vm;
        vm->cores[i].threads =
            malloc(threads_per_core * sizeof(*vm->cores[i].threads));
        if (vm->cores[i].threads == NULL) {
            vm->core_count = i;
            release_core_storage(vm);
            return 0;
        }
    }

    size_t logical_index = 0;
    for (size_t core_index = 0; core_index < core_count; ++core_index) {
        Core *core = &vm->cores[core_index];
        for (size_t thread_index = 0;
             thread_index < threads_per_core;
             ++thread_index, ++logical_index) {
            HardwareThread *thread = &core->threads[thread_index];
            if (!cpu_init(&thread->cpu, ram)) {
                release_core_storage(vm);
                return 0;
            }

            thread->cpu.core_id = (uint32_t)core_index;
            thread->cpu.thread_id = (uint32_t)thread_index;
            interrupt_controller_init(&thread->interrupts);
            atomic_init(&thread->state, HARDWARE_THREAD_OFFLINE);
            atomic_init(&thread->stop_requested, 0);
            vm->interrupt_targets[logical_index] = &thread->interrupts;
        }
    }

    HardwareThread *boot_thread = &vm->cores[0].threads[0];
    atomic_store_explicit(&boot_thread->state,
                          HARDWARE_THREAD_RUNNABLE,
                          memory_order_release);

    if (!interrupt_router_init(&vm->interrupt_router,
                               vm->interrupt_targets,
                               logical_processors)) {
        release_core_storage(vm);
        return 0;
    }
    return 1;
}

void vm_destroy(VirtualMachine *vm)
{
    if (vm == NULL) {
        return;
    }

    release_core_storage(vm);
    *vm = (VirtualMachine){0};
}

static int run_workers(VirtualMachine *vm)
{
    atomic_store_explicit(&vm->stop_requested, 0, memory_order_release);
    atomic_store_explicit(&vm->failed, 0, memory_order_release);

    if (!host_thread_create(&vm->device_worker, device_worker, vm)) {
        return 0;
    }

    size_t started = 0;
    for (; started < vm->core_count; ++started) {
        if (!host_thread_create(&vm->cores[started].worker,
                                core_worker,
                                &vm->cores[started])) {
            atomic_store_explicit(&vm->failed, 1, memory_order_release);
            atomic_store_explicit(&vm->stop_requested,
                                  1,
                                  memory_order_release);
            break;
        }
    }

    for (size_t i = 0; i < started; ++i) {
        int worker_result = 0;
        if (!host_thread_join(&vm->cores[i].worker, &worker_result) ||
            !worker_result) {
            atomic_store_explicit(&vm->failed, 1, memory_order_release);
            atomic_store_explicit(&vm->stop_requested,
                                  1,
                                  memory_order_release);
        }
    }

    atomic_store_explicit(&vm->stop_requested, 1, memory_order_release);
    int device_result = 0;
    if (!host_thread_join(&vm->device_worker, &device_result) ||
        !device_result) {
        atomic_store_explicit(&vm->failed, 1, memory_order_release);
    }

    return !atomic_load_explicit(&vm->failed, memory_order_acquire);
}

static int prepare_warm_reset(VirtualMachine *vm)
{
    bus_reset(vm->bus);

    for (size_t core_index = 0;
         core_index < vm->core_count;
         ++core_index) {
        Core *core = &vm->cores[core_index];
        core->next_thread = 0;
        for (size_t thread_index = 0;
             thread_index < core->thread_count;
             ++thread_index) {
            HardwareThread *thread = &core->threads[thread_index];
            if (!cpu_init(&thread->cpu, vm->ram)) {
                return 0;
            }
            thread->cpu.core_id = (uint32_t)core_index;
            thread->cpu.thread_id = (uint32_t)thread_index;
            interrupt_controller_init(&thread->interrupts);
            atomic_store_explicit(&thread->state,
                                  HARDWARE_THREAD_OFFLINE,
                                  memory_order_release);
            atomic_store_explicit(&thread->stop_requested,
                                  0,
                                  memory_order_release);
        }
    }

    HardwareThread *boot_thread = &vm->cores[0].threads[0];
    boot_thread->cpu.pc = vm->reset_vector;
    atomic_store_explicit(&boot_thread->state,
                          HARDWARE_THREAD_RUNNABLE,
                          memory_order_release);
    atomic_store_explicit(&vm->active_thread_count,
                          1,
                          memory_order_release);
    atomic_store_explicit(&vm->system_action,
                          VM_SYSTEM_ACTION_NONE,
                          memory_order_release);
    atomic_store_explicit(&vm->stop_requested, 0, memory_order_release);
    atomic_store_explicit(&vm->failed, 0, memory_order_release);
    return 1;
}

int vm_run(VirtualMachine *vm)
{
    if (vm == NULL || vm->cores == NULL || vm->core_count == 0 ||
        atomic_load_explicit(&vm->active_thread_count,
                             memory_order_acquire) == 0 ||
        vm_system_action(vm) != VM_SYSTEM_ACTION_NONE) {
        return 0;
    }

    for (;;) {
        if (!run_workers(vm)) {
            return 0;
        }

        VmSystemAction action = vm_system_action(vm);
        if (action != VM_SYSTEM_ACTION_WARM_RESET) {
            return action == VM_SYSTEM_ACTION_NONE ||
                   action == VM_SYSTEM_ACTION_SHUTDOWN;
        }
        if (!prepare_warm_reset(vm)) {
            atomic_store_explicit(&vm->failed, 1, memory_order_release);
            return 0;
        }
    }
}

int vm_set_reset_vector(VirtualMachine *vm, uint64_t reset_vector)
{
    uint64_t first_instruction;
    if (vm == NULL || vm->bus == NULL ||
        !bus_fetch(vm->bus, reset_vector, 1, &first_instruction)) {
        return 0;
    }

    vm->reset_vector = reset_vector;
    HardwareThread *boot_thread = vm_hardware_thread(vm, 0, 0);
    if (boot_thread != NULL &&
        atomic_load_explicit(&boot_thread->state,
                             memory_order_acquire) ==
            HARDWARE_THREAD_RUNNABLE) {
        boot_thread->cpu.pc = reset_vector;
    }
    return 1;
}

int vm_request_system_action(VirtualMachine *vm, VmSystemAction action)
{
    if (vm == NULL ||
        (action != VM_SYSTEM_ACTION_SHUTDOWN &&
         action != VM_SYSTEM_ACTION_WARM_RESET)) {
        return 0;
    }

    int expected = VM_SYSTEM_ACTION_NONE;
    if (!atomic_compare_exchange_strong_explicit(
            &vm->system_action,
            &expected,
            action,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return 0;
    }
    atomic_store_explicit(&vm->stop_requested, 1, memory_order_release);
    return 1;
}

VmSystemAction vm_system_action(const VirtualMachine *vm)
{
    if (vm == NULL) {
        return VM_SYSTEM_ACTION_NONE;
    }
    return (VmSystemAction)atomic_load_explicit(&vm->system_action,
                                                memory_order_acquire);
}

int vm_activate_hardware_thread(VirtualMachine *vm,
                                size_t core_id,
                                size_t thread_id,
                                uint64_t entry_address)
{
    HardwareThread *thread = vm_hardware_thread(vm, core_id, thread_id);
    uint64_t first_instruction_byte;
    if (thread == NULL ||
        !bus_fetch(vm->bus,
                   entry_address,
                   1,
                   &first_instruction_byte) ||
        atomic_load_explicit(&vm->stop_requested,
                             memory_order_acquire)) {
        return 0;
    }

    int expected_state = HARDWARE_THREAD_OFFLINE;
    if (!atomic_compare_exchange_strong_explicit(
            &thread->state,
            &expected_state,
            HARDWARE_THREAD_STARTING,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return 0;
    }

    uint32_t saved_core_id = thread->cpu.core_id;
    uint32_t saved_thread_id = thread->cpu.thread_id;
    if (!cpu_init(&thread->cpu, vm->ram)) {
        atomic_store_explicit(&thread->state,
                              HARDWARE_THREAD_OFFLINE,
                              memory_order_release);
        return 0;
    }

    thread->cpu.core_id = saved_core_id;
    thread->cpu.thread_id = saved_thread_id;
    thread->cpu.pc = entry_address;
    interrupt_controller_init(&thread->interrupts);
    atomic_store_explicit(&thread->stop_requested,
                          0,
                          memory_order_release);

    atomic_fetch_add_explicit(&vm->active_thread_count,
                              1,
                              memory_order_acq_rel);
    atomic_store_explicit(&thread->state,
                          HARDWARE_THREAD_RUNNABLE,
                          memory_order_release);
    return 1;
}

int vm_request_hardware_thread_stop(VirtualMachine *vm,
                                    size_t core_id,
                                    size_t thread_id)
{
    HardwareThread *thread = vm_hardware_thread(vm, core_id, thread_id);
    if (thread == NULL) {
        return 0;
    }

    int state = atomic_load_explicit(&thread->state,
                                     memory_order_acquire);
    if (state != HARDWARE_THREAD_RUNNABLE &&
        state != HARDWARE_THREAD_WAITING) {
        return 0;
    }

    atomic_store_explicit(&thread->stop_requested,
                          1,
                          memory_order_release);
    return 1;
}

int vm_reset_hardware_thread(VirtualMachine *vm,
                             size_t core_id,
                             size_t thread_id)
{
    HardwareThread *thread = vm_hardware_thread(vm, core_id, thread_id);
    if (thread == NULL) {
        return 0;
    }

    int state = atomic_load_explicit(&thread->state,
                                     memory_order_acquire);
    for (;;) {
        if (state != HARDWARE_THREAD_OFFLINE &&
            state != HARDWARE_THREAD_HALTED) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &thread->state,
                &state,
                HARDWARE_THREAD_STARTING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }

    uint32_t saved_core_id = thread->cpu.core_id;
    uint32_t saved_thread_id = thread->cpu.thread_id;
    if (!cpu_init(&thread->cpu, vm->ram)) {
        atomic_store_explicit(&thread->state,
                              state,
                              memory_order_release);
        return 0;
    }

    thread->cpu.core_id = saved_core_id;
    thread->cpu.thread_id = saved_thread_id;
    interrupt_controller_init(&thread->interrupts);
    atomic_store_explicit(&thread->stop_requested,
                          0,
                          memory_order_release);
    atomic_store_explicit(&thread->state,
                          HARDWARE_THREAD_OFFLINE,
                          memory_order_release);
    return 1;
}

HardwareThread *vm_hardware_thread(VirtualMachine *vm,
                                   size_t core_id,
                                   size_t thread_id)
{
    if (vm == NULL || core_id >= vm->core_count ||
        thread_id >= vm->threads_per_core) {
        return NULL;
    }
    return &vm->cores[core_id].threads[thread_id];
}

int vm_set_vector_base(VirtualMachine *vm,
                       size_t logical_processor,
                       uint64_t address)
{
    if (vm == NULL || logical_processor >= vm->logical_processor_count) {
        return 0;
    }

    size_t core_id = logical_processor / vm->threads_per_core;
    size_t thread_id = logical_processor % vm->threads_per_core;
    HardwareThread *thread = vm_hardware_thread(vm, core_id, thread_id);
    return thread != NULL &&
           cpu_set_vector_base(&thread->cpu, vm->ram, address);
}

int vm_raise_hardware_thread_interrupt(VirtualMachine *vm,
                                       size_t core_id,
                                       size_t thread_id,
                                       unsigned int line)
{
    HardwareThread *thread = vm_hardware_thread(vm, core_id, thread_id);
    if (thread == NULL || line < INTERRUPT_IPI_LINE_BASE ||
        line >= INTERRUPT_LINE_COUNT) {
        return 0;
    }

    int state = atomic_load_explicit(&thread->state,
                                     memory_order_acquire);
    if (state != HARDWARE_THREAD_RUNNABLE &&
        state != HARDWARE_THREAD_WAITING) {
        return 0;
    }
    return interrupt_controller_raise(&thread->interrupts, line);
}

int vm_route_interrupt(VirtualMachine *vm,
                       unsigned int line,
                       size_t logical_processor)
{
    if (vm == NULL) {
        return 0;
    }
    return interrupt_router_set_route(&vm->interrupt_router,
                                      line,
                                      logical_processor);
}
