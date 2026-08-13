#include "kernel_internal.h"

#include "core_control_protocol.h"
#include "system_info_protocol.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

#define KERNEL_SMP_MAX_CORES UINT64_C(64)
#define KERNEL_SMP_MAX_THREADS_PER_CORE UINT64_C(8)
#define KERNEL_SMP_MAX_CPUS \
    (KERNEL_SMP_MAX_CORES * KERNEL_SMP_MAX_THREADS_PER_CORE)
#define KERNEL_SMP_STACK_SIZE ((size_t)0x4000)
#define KERNEL_SMP_WAIT_LIMIT UINT64_C(2000000)
#define KERNEL_SMP_IPI_LINE ((uint64_t)INTERRUPT_IPI_LINE_BASE)

#define KERNEL_CPU_OFFLINE UINT64_C(0)
#define KERNEL_CPU_BOOTING UINT64_C(1)
#define KERNEL_CPU_ONLINE UINT64_C(2)
#define KERNEL_CPU_WAITING UINT64_C(3)
#define KERNEL_CPU_STOPPED UINT64_C(4)

typedef struct {
    uint64_t stack_top;
    volatile uint64_t state;
    volatile uint64_t current_thread;
    volatile uint64_t current_process;
    volatile uint64_t kernel_stack_thread;
    volatile uint64_t kernel_stack_process;
    volatile uint64_t trap_count;
    volatile uint64_t ipi_count;
    volatile uint64_t idle_count;
    volatile uint64_t kernel_lock_held;
    volatile uint64_t idle_active;
} KernelCpuLocal;

typedef struct {
    uint64_t threads_per_core;
    uint64_t logical_processor_count;
    uint64_t page_table_root;
    uint64_t vector_base;
    uint64_t secondary_entry;
    KernelCpuLocal cpus[KERNEL_SMP_MAX_CPUS];
} KernelSmpBootstrap;

static KernelSmpBootstrap kernel_smp_bootstrap;
static uint8_t *kernel_smp_stacks[KERNEL_SMP_MAX_CPUS];
static int kernel_smp_initialized;
static KernelSpinLock kernel_smp_big_lock;

static int smp_map_mmio(uintptr_t alias, uintptr_t physical, uint64_t size)
{
    if ((alias & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        (physical & (uintptr_t)KERNEL_PAGE_MASK) != 0 || size == 0 ||
        size > KERNEL_PAGE_SIZE) {
        return 0;
    }
    return kernel_map_page(kernel_page_table_root(), alias, physical,
                           KERNEL_PTE_VALID | KERNEL_PTE_READ |
                               KERNEL_PTE_WRITE) == 0;
}

static void smp_select(uint64_t core, uint64_t thread)
{
    cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                         CORE_CONTROL_TARGET_CORE_OFFSET,
                     core);
    cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                         CORE_CONTROL_TARGET_THREAD_OFFSET,
                     thread);
}

static uint64_t smp_status(void)
{
    return cvm_mmio_read64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                           CORE_CONTROL_STATUS_OFFSET);
}

static int smp_target(uint64_t logical, uint64_t *core, uint64_t *thread)
{
    uint64_t threads = kernel_smp_bootstrap.threads_per_core;
    if (core == NULL || thread == NULL || threads == 0 ||
        logical >= kernel_smp_bootstrap.logical_processor_count) {
        return 0;
    }
    *core = logical / threads;
    *thread = logical % threads;
    return 1;
}

static KernelCpuLocal *smp_current_cpu(void)
{
    uint64_t threads = kernel_smp_bootstrap.threads_per_core;
    uint64_t logical_count =
        kernel_smp_bootstrap.logical_processor_count;
    uint64_t core = cvm_core_id();
    uint64_t thread = cvm_thread_id();
    if (!kernel_smp_initialized || threads == 0 ||
        core > UINT64_MAX / threads) {
        return NULL;
    }
    uint64_t logical = core * threads + thread;
    if (logical >= logical_count || logical >= KERNEL_SMP_MAX_CPUS) {
        return NULL;
    }
    return &kernel_smp_bootstrap.cpus[logical];
}

static uint64_t smp_current_logical(void)
{
    uint64_t threads = kernel_smp_bootstrap.threads_per_core;
    uint64_t core = cvm_core_id();
    uint64_t thread = cvm_thread_id();
    if (threads == 0 || core > UINT64_MAX / threads) return UINT64_MAX;
    uint64_t logical = core * threads + thread;
    return logical < kernel_smp_bootstrap.logical_processor_count
               ? logical : UINT64_MAX;
}

static int smp_wait_for_target(uint64_t logical,
                               uint64_t guest_state,
                               uint64_t hardware_state,
                               uint64_t minimum_ipi_count)
{
    uint64_t core;
    uint64_t thread;
    if (!smp_target(logical, &core, &thread)) return 0;
    smp_select(core, thread);
    KernelCpuLocal *cpu = &kernel_smp_bootstrap.cpus[logical];
    for (uint64_t wait = 0; wait < KERNEL_SMP_WAIT_LIMIT; ++wait) {
        cvm_fence();
        if (cpu->state == guest_state &&
            cpu->ipi_count >= minimum_ipi_count &&
            smp_status() == hardware_state) {
            return 1;
        }
        cvm_nop();
    }
    return 0;
}

static int smp_wait_for_halted(uint64_t logical)
{
    uint64_t core;
    uint64_t thread;
    if (!smp_target(logical, &core, &thread)) return 0;
    smp_select(core, thread);
    for (uint64_t wait = 0; wait < KERNEL_SMP_WAIT_LIMIT; ++wait) {
        if (smp_status() == CORE_CONTROL_STATUS_HALTED) return 1;
        cvm_nop();
    }
    return 0;
}

static int smp_patch_and_map_trampoline(void)
{
    uintptr_t source = kernel_symbol_physical_address(
        kernel_secondary_trampoline);
    uintptr_t source_end = kernel_symbol_physical_address(
        kernel_secondary_trampoline_end);
    uintptr_t load = kernel_symbol_physical_address(
        kernel_secondary_table_load);
    uintptr_t table = kernel_symbol_physical_address(&kernel_smp_bootstrap);
    if (source == 0 || source_end <= source || load == 0 || table == 0 ||
        load < source || source_end - source > KERNEL_PAGE_SIZE ||
        load - source > KERNEL_PAGE_SIZE - 10) {
        return 0;
    }
    const uint8_t *source_bytes = kernel_phys_to_virt(source);
    uint8_t *trampoline = kernel_phys_to_virt(
        (uintptr_t)KERNEL_SMP_TRAMPOLINE_ADDRESS);
    if (source_bytes == NULL || trampoline == NULL) return 0;
    size_t trampoline_size = (size_t)(source_end - source);
    for (size_t i = 0; i < trampoline_size; ++i) {
        trampoline[i] = source_bytes[i];
    }
    uint8_t *instruction = trampoline + (load - source);
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        instruction[2 + i] = (uint8_t)((uint64_t)table >> (i * 8));
    }
    cvm_fence();
    return kernel_map_page(kernel_page_table_root(),
                           (uintptr_t)KERNEL_SMP_TRAMPOLINE_ADDRESS,
                           (uintptr_t)KERNEL_SMP_TRAMPOLINE_ADDRESS,
                           KERNEL_PTE_VALID | KERNEL_PTE_READ |
                               KERNEL_PTE_EXECUTE) == 0;
}

static int smp_send_ipi(uint64_t logical)
{
    uint64_t core;
    uint64_t thread;
    if (!smp_target(logical, &core, &thread)) return 0;
    smp_select(core, thread);
    cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                         CORE_CONTROL_IPI_LINE_OFFSET,
                     KERNEL_SMP_IPI_LINE);
    cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                         CORE_CONTROL_COMMAND_OFFSET,
                     CORE_CONTROL_COMMAND_IPI);
    return cvm_mmio_read64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                           CORE_CONTROL_RESULT_OFFSET) ==
           CORE_CONTROL_RESULT_SUCCESS;
}

static int smp_start_secondary(uint64_t logical)
{
    uint64_t core;
    uint64_t thread;
    if (!smp_target(logical, &core, &thread) || logical == 0) return 0;
    uint8_t *stack = kernel_malloc(KERNEL_SMP_STACK_SIZE);
    if (stack == NULL) return 0;
    uintptr_t stack_top = (uintptr_t)(stack + KERNEL_SMP_STACK_SIZE) &
                          ~(uintptr_t)UINT64_C(0xF);
    KernelCpuLocal *cpu = &kernel_smp_bootstrap.cpus[logical];
    kernel_smp_stacks[logical] = stack;
    cpu->stack_top = stack_top;
    cpu->state = KERNEL_CPU_BOOTING;
    cpu->current_thread = 0;
    cpu->current_process = 0;
    cpu->kernel_stack_thread = 0;
    cpu->kernel_stack_process = 0;
    cpu->trap_count = 0;
    cpu->ipi_count = 0;
    cpu->idle_count = 0;
    cpu->kernel_lock_held = 0;
    cpu->idle_active = 0;
    cvm_fence();

    smp_select(core, thread);
    cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                         CORE_CONTROL_ENTRY_PC_OFFSET,
                     KERNEL_SMP_TRAMPOLINE_ADDRESS);
    cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                         CORE_CONTROL_COMMAND_OFFSET,
                     CORE_CONTROL_COMMAND_START);
    if (cvm_mmio_read64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                        CORE_CONTROL_RESULT_OFFSET) !=
            CORE_CONTROL_RESULT_SUCCESS ||
        !smp_wait_for_target(logical, KERNEL_CPU_WAITING,
                             CORE_CONTROL_STATUS_WAITING, 0) ||
        !smp_send_ipi(logical) ||
        !smp_wait_for_target(logical, KERNEL_CPU_WAITING,
                             CORE_CONTROL_STATUS_WAITING, 1)) {
        smp_select(core, thread);
        cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                             CORE_CONTROL_COMMAND_OFFSET,
                         CORE_CONTROL_COMMAND_STOP);
        (void)smp_wait_for_halted(logical);
        cpu->state = KERNEL_CPU_STOPPED;
        cpu->stack_top = 0;
        kernel_smp_stacks[logical] = NULL;
        kernel_free(stack);
        return 0;
    }
    return 1;
}

void kernel_secondary_main(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL || cpu->stack_top == 0) cvm_halt();
    cpu->state = KERNEL_CPU_ONLINE;
    cvm_fence();
    for (;;) {
        if (kernel_scheduler_running()) kernel_scheduler_secondary_start();
        kernel_smp_idle_enter();
        kernel_idle_wait();
        kernel_smp_idle_leave();
    }
}

void kernel_smp_set_current_thread(KernelThread *thread)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu != NULL) {
        cpu->current_thread = (uint64_t)(uintptr_t)thread;
        cpu->current_process = thread != NULL
            ? (uint64_t)(uintptr_t)thread->process : 0;
    }
}

KernelThread *kernel_smp_current_thread(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    return cpu != NULL
               ? (KernelThread *)(uintptr_t)cpu->current_thread
               : NULL;
}

int kernel_smp_thread_active(const KernelThread *thread)
{
    if (thread == NULL || !kernel_smp_initialized) return 0;
    uint64_t address = (uint64_t)(uintptr_t)thread;
    for (uint64_t index = 0;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        const KernelCpuLocal *cpu = &kernel_smp_bootstrap.cpus[index];
        if (cpu->current_thread == address ||
            cpu->kernel_stack_thread == address) {
            return 1;
        }
    }
    return 0;
}

int kernel_smp_process_active(const KernelProcess *process)
{
    if (process == NULL || !kernel_smp_initialized) return 0;
    for (uint64_t index = 0;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        const KernelCpuLocal *cpu = &kernel_smp_bootstrap.cpus[index];
        if (cpu->current_process == (uint64_t)(uintptr_t)process ||
            cpu->kernel_stack_process == (uint64_t)(uintptr_t)process) {
            return 1;
        }
    }
    return 0;
}

int kernel_smp_process_other_thread_active(const KernelProcess *process,
                                           const KernelThread *owner)
{
    if (process == NULL || !kernel_smp_initialized) return 0;
    uint64_t process_address = (uint64_t)(uintptr_t)process;
    uint64_t owner_address = (uint64_t)(uintptr_t)owner;
    for (uint64_t index = 0;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        const KernelCpuLocal *cpu = &kernel_smp_bootstrap.cpus[index];
        if ((cpu->current_process == process_address &&
             cpu->current_thread != owner_address) ||
            (cpu->kernel_stack_process == process_address &&
             cpu->kernel_stack_thread != owner_address)) {
            return 1;
        }
    }
    return 0;
}

int kernel_smp_has_running_thread(void)
{
    if (!kernel_smp_initialized) return 0;
    for (uint64_t index = 0;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        KernelThread *thread = (KernelThread *)(uintptr_t)
            kernel_smp_bootstrap.cpus[index].current_thread;
        if (thread != NULL && thread->state == KERNEL_THREAD_RUNNING) return 1;
    }
    return 0;
}

uint64_t kernel_smp_logical_count(void)
{
    return kernel_smp_initialized
               ? kernel_smp_bootstrap.logical_processor_count : 0;
}

uint64_t kernel_smp_current_logical_id(void)
{
    return smp_current_logical();
}

int kernel_smp_is_boot_cpu(void)
{
    return cvm_core_id() == 0 && cvm_thread_id() == 0;
}

int kernel_smp_idle_active(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    return cpu != NULL && cpu->idle_active != 0;
}

void kernel_smp_idle_enter(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL) return;
    cpu->idle_active = 1;
    cpu->state = KERNEL_CPU_WAITING;
    (void)cvm_atomic_add64(&cpu->idle_count, 1);
    cvm_fence();
}

void kernel_smp_idle_leave(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL) return;
    cpu->idle_active = 0;
    cpu->state = KERNEL_CPU_ONLINE;
    cvm_fence();
}

void kernel_smp_trap_enter(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL) return;
    kernel_spin_lock(&kernel_smp_big_lock);
    cpu->kernel_lock_held = 1;
    cpu->kernel_stack_thread = cpu->current_thread;
    cpu->kernel_stack_process = cpu->current_process;
    (void)cvm_atomic_add64(&cpu->trap_count, 1);
    cpu->state = KERNEL_CPU_ONLINE;
}

void kernel_smp_trap_leave(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL || cpu->kernel_lock_held == 0) return;
    cpu->kernel_lock_held = 0;
    kernel_spin_unlock(&kernel_smp_big_lock);
}

void kernel_smp_switch_release(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL || cpu->kernel_lock_held == 0) return;
    cpu->kernel_lock_held = 0;
    kernel_spin_unlock(&kernel_smp_big_lock);
}

void kernel_smp_switch_reacquire(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL || cpu->kernel_lock_held != 0) return;
    kernel_spin_lock(&kernel_smp_big_lock);
    cpu->kernel_lock_held = 1;
    cpu->kernel_stack_thread = cpu->current_thread;
    cpu->kernel_stack_process = cpu->current_process;
}

void kernel_smp_ipi_interrupt(uint64_t *frame)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu != NULL) (void)cvm_atomic_add64(&cpu->ipi_count, 1);
    if (kernel_scheduler_running()) {
        kernel_scheduler_ipi(frame);
    } else {
        kernel_scheduler_interrupt_return(frame);
    }
}

int kernel_smp_kick_secondaries(void)
{
    if (!kernel_smp_initialized) return 0;
    int okay = 1;
    for (uint64_t index = 1;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        if (!smp_send_ipi(index)) okay = 0;
    }
    return okay;
}

int kernel_smp_wake_idle_other(void)
{
    if (!kernel_smp_initialized) return 0;
    uint64_t current = smp_current_logical();
    for (uint64_t index = 0;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        if (index != current &&
            kernel_smp_bootstrap.cpus[index].idle_active != 0) {
            return smp_send_ipi(index);
        }
    }
    return 0;
}

int kernel_smp_wake_logical(uint64_t logical)
{
    return kernel_smp_initialized && logical != smp_current_logical() &&
           smp_send_ipi(logical);
}

void kernel_smp_reschedule_others(void)
{
    if (!kernel_smp_initialized) return;
    uint64_t current = smp_current_logical();
    for (uint64_t index = 0;
         index < kernel_smp_bootstrap.logical_processor_count;
         ++index) {
        if (index != current) (void)smp_send_ipi(index);
    }
}

int kernel_smp_wake_boot_cpu(void)
{
    return kernel_smp_initialized && smp_current_logical() != 0 &&
           smp_send_ipi(0);
}

void kernel_smp_secondary_retire(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL) cvm_halt();
    kernel_secondary_restart((uintptr_t)cpu->stack_top);
    cvm_halt();
}

void kernel_smp_retire_finalize(void)
{
    KernelCpuLocal *cpu = smp_current_cpu();
    if (cpu == NULL) cvm_halt();
    int boot_cpu = kernel_smp_is_boot_cpu();
    kernel_smp_switch_reacquire();
    cpu->current_thread = 0;
    cpu->current_process = 0;
    cpu->kernel_stack_thread = 0;
    cpu->kernel_stack_process = 0;
    cpu->idle_active = 0;
    cpu->state = KERNEL_CPU_ONLINE;
    if (!boot_cpu && !smp_send_ipi(0)) cvm_halt();
    kernel_smp_switch_release();
}

int kernel_smp_runtime_valid(void)
{
    if (!kernel_smp_initialized) return 0;
    uint64_t logical = kernel_smp_bootstrap.logical_processor_count;
    for (uint64_t index = 0; index < logical; ++index) {
        KernelCpuLocal *cpu = &kernel_smp_bootstrap.cpus[index];
        if (cpu->stack_top == 0) return 0;
        if (index == 0) {
            if (cpu->state != KERNEL_CPU_ONLINE &&
                cpu->state != KERNEL_CPU_WAITING) {
                return 0;
            }
        } else if ((cpu->state != KERNEL_CPU_ONLINE &&
                    cpu->state != KERNEL_CPU_WAITING) ||
                   cpu->ipi_count == 0 || cpu->idle_count == 0 ||
                   kernel_smp_stacks[index] == NULL) {
            return 0;
        }
    }
    return 1;
}

int kernel_smp_shutdown_secondary(void)
{
    if (!kernel_smp_initialized) return 0;
    int failed = 0;
    uint64_t logical = kernel_smp_bootstrap.logical_processor_count;
    for (uint64_t index = 1; index < logical; ++index) {
        if (kernel_smp_stacks[index] == NULL) continue;
        uint64_t core;
        uint64_t thread;
        if (!smp_target(index, &core, &thread)) {
            failed = 1;
            continue;
        }
        smp_select(core, thread);
        cvm_mmio_write64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                             CORE_CONTROL_COMMAND_OFFSET,
                         CORE_CONTROL_COMMAND_STOP);
        if (cvm_mmio_read64((uintptr_t)KERNEL_CORE_CONTROL_ALIAS +
                            CORE_CONTROL_RESULT_OFFSET) !=
                CORE_CONTROL_RESULT_SUCCESS ||
            !smp_wait_for_halted(index)) {
            failed = 1;
            continue;
        }
        kernel_smp_bootstrap.cpus[index].state = KERNEL_CPU_STOPPED;
        kernel_smp_bootstrap.cpus[index].current_thread = 0;
        kernel_smp_bootstrap.cpus[index].current_process = 0;
        kernel_smp_bootstrap.cpus[index].kernel_stack_thread = 0;
        kernel_smp_bootstrap.cpus[index].kernel_stack_process = 0;
        kernel_smp_bootstrap.cpus[index].stack_top = 0;
        kernel_free(kernel_smp_stacks[index]);
        kernel_smp_stacks[index] = NULL;
    }
    if (!failed) kernel_smp_initialized = 0;
    return failed;
}

int kernel_smp_bootstrap_test(uint64_t boot_thread_id)
{
    if (sizeof(KernelCpuLocal) != 88 ||
        offsetof(KernelSmpBootstrap, cpus) != 40 ||
        boot_thread_id != 0 || cvm_core_id() != 0 || cvm_thread_id() != 0 ||
        !smp_map_mmio((uintptr_t)KERNEL_SYSTEM_INFO_ALIAS,
                      (uintptr_t)kernel_boot_info->system_info_base,
                      SYSTEM_INFO_MMIO_SIZE) ||
        !smp_map_mmio((uintptr_t)KERNEL_CORE_CONTROL_ALIAS,
                      (uintptr_t)kernel_boot_info->core_control_base,
                      CORE_CONTROL_MMIO_SIZE) ||
        cvm_mmio_read64((uintptr_t)KERNEL_SYSTEM_INFO_ALIAS +
                        SYSTEM_INFO_MAGIC_OFFSET) != SYSTEM_INFO_MAGIC ||
        cvm_mmio_read64((uintptr_t)KERNEL_SYSTEM_INFO_ALIAS +
                        SYSTEM_INFO_VERSION_OFFSET) != SYSTEM_INFO_VERSION) {
        return 1;
    }

    uint64_t cores = cvm_mmio_read64(
        (uintptr_t)KERNEL_SYSTEM_INFO_ALIAS + SYSTEM_INFO_CORE_COUNT_OFFSET);
    uint64_t threads = cvm_mmio_read64(
        (uintptr_t)KERNEL_SYSTEM_INFO_ALIAS +
        SYSTEM_INFO_THREADS_PER_CORE_OFFSET);
    uint64_t logical = cvm_mmio_read64(
        (uintptr_t)KERNEL_SYSTEM_INFO_ALIAS +
        SYSTEM_INFO_LOGICAL_PROCESSORS_OFFSET);
    if (cores == 0 || cores > KERNEL_SMP_MAX_CORES || threads == 0 ||
        threads > KERNEL_SMP_MAX_THREADS_PER_CORE ||
        cores > UINT64_MAX / threads || logical != cores * threads ||
        logical > KERNEL_SMP_MAX_CPUS) {
        return 1;
    }

    kernel_smp_bootstrap.threads_per_core = threads;
    kernel_smp_bootstrap.logical_processor_count = logical;
    kernel_smp_bootstrap.page_table_root = kernel_page_table_root();
    kernel_smp_bootstrap.vector_base = kernel_vector_base();
    kernel_smp_bootstrap.secondary_entry =
        (uint64_t)(uintptr_t)kernel_secondary_main;
    for (uint64_t index = 0; index < logical; ++index) {
        kernel_smp_stacks[index] = NULL;
        kernel_smp_bootstrap.cpus[index].stack_top = 0;
        kernel_smp_bootstrap.cpus[index].state = KERNEL_CPU_OFFLINE;
        kernel_smp_bootstrap.cpus[index].current_thread = 0;
        kernel_smp_bootstrap.cpus[index].current_process = 0;
        kernel_smp_bootstrap.cpus[index].kernel_stack_thread = 0;
        kernel_smp_bootstrap.cpus[index].kernel_stack_process = 0;
        kernel_smp_bootstrap.cpus[index].trap_count = 0;
        kernel_smp_bootstrap.cpus[index].ipi_count = 0;
        kernel_smp_bootstrap.cpus[index].idle_count = 0;
        kernel_smp_bootstrap.cpus[index].kernel_lock_held = 0;
        kernel_smp_bootstrap.cpus[index].idle_active = 0;
    }
    kernel_smp_bootstrap.cpus[0].stack_top =
        (uint64_t)(uintptr_t)kernel_stack_top;
    kernel_smp_bootstrap.cpus[0].state = KERNEL_CPU_ONLINE;
    kernel_spin_init(&kernel_smp_big_lock);
    kernel_smp_initialized = 1;
    if (logical == 1) return 0;
    if (!smp_patch_and_map_trampoline()) {
        kernel_smp_initialized = 0;
        return 1;
    }
    for (uint64_t index = 1; index < logical; ++index) {
        if (!smp_start_secondary(index)) {
            (void)kernel_smp_shutdown_secondary();
            return 1;
        }
    }
    return kernel_smp_runtime_valid() ? 0 : 1;
}
