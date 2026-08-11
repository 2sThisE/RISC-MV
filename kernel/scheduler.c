#include "kernel_internal.h"

#include "builtin_device_protocol.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

#define KERNEL_TIMER_QUANTUM 2U
#define KERNEL_SCHEDULER_TEST_PROCESSES 3U
#define KERNEL_SCHEDULER_TEST_THREADS 4U

#define TRAP_REGISTER(frame, reg) ((frame)[15U - (reg)])
#define TRAP_PC(frame) ((frame)[16])
#define TRAP_FLAGS(frame) ((frame)[17])
#define TRAP_USER_SP(frame) ((frame)[18])

static KernelList scheduler_processes;
static KernelList scheduler_run_queue;
static KernelList scheduler_reap_queue;
static KernelThread *scheduler_current;
static size_t scheduler_process_count;
static size_t scheduler_thread_count;
static uint64_t scheduler_timer_count;
static uint64_t scheduler_preemption_count;
static size_t scheduler_completed_process_count;
static size_t scheduler_faulted_process_count;
static size_t scheduler_reaped_process_count;
static int scheduler_completion_error;
static int scheduler_active;

static KernelProcess *scheduler_process_from_node(KernelListNode *node)
{
    return (KernelProcess *)((uint8_t *)node -
                             offsetof(KernelProcess, scheduler_node));
}

static KernelThread *scheduler_process_thread_from_node(KernelListNode *node)
{
    return (KernelThread *)((uint8_t *)node -
                            offsetof(KernelThread, process_node));
}

static KernelThread *scheduler_run_thread_from_node(KernelListNode *node)
{
    return (KernelThread *)((uint8_t *)node -
                            offsetof(KernelThread, run_node));
}

static KernelProcess *scheduler_reap_process_from_node(KernelListNode *node)
{
    return (KernelProcess *)((uint8_t *)node -
                             offsetof(KernelProcess, reap_node));
}

static void scheduler_save(KernelThread *thread, const uint64_t *frame)
{
    for (size_t reg = 0; reg < 15; ++reg) {
        thread->registers[reg] = TRAP_REGISTER(frame, reg);
    }
    thread->pc = TRAP_PC(frame);
    thread->flags = TRAP_FLAGS(frame) & UINT64_C(0x3F);
    thread->stack_pointer = TRAP_USER_SP(frame);
}

static void scheduler_load(const KernelThread *previous,
                           const KernelThread *thread,
                           uint64_t *frame)
{
    uint64_t exception_frame = TRAP_FLAGS(frame) &
                               KERNEL_SAVED_EXCEPTION_FRAME;
    for (size_t reg = 0; reg < 15; ++reg) {
        TRAP_REGISTER(frame, reg) = thread->registers[reg];
    }
    TRAP_PC(frame) = thread->pc;
    TRAP_FLAGS(frame) = thread->flags | KERNEL_SAVED_USER_MODE |
                        KERNEL_CPU_FLAG_INTERRUPT_ENABLE |
                        exception_frame;
    TRAP_USER_SP(frame) = thread->stack_pointer;
    if (previous == NULL ||
        previous->process->image.address_space !=
            thread->process->image.address_space) {
        cvm_set_ptbr((uint64_t)kernel_address_space_root(
            thread->process->image.address_space));
    }
    cvm_set_ksp((uint64_t)thread->kernel_stack_top);
}

static int scheduler_enqueue(KernelThread *thread)
{
    if (thread == NULL || thread->queued ||
        thread->state != KERNEL_THREAD_RUNNABLE) {
        return 0;
    }
    kernel_list_push_back(&scheduler_run_queue, &thread->run_node);
    thread->queued = 1;
    return 1;
}

static KernelThread *scheduler_dequeue(void)
{
    KernelListNode *node = kernel_list_pop_front(&scheduler_run_queue);
    if (node == NULL) return NULL;
    KernelThread *thread = scheduler_run_thread_from_node(node);
    thread->queued = 0;
    thread->state = KERNEL_THREAD_RUNNING;
    return thread;
}

static int scheduler_register_process(KernelProcess *process)
{
    if (process == NULL || process->thread_count == 0) return 0;
    kernel_list_push_back(&scheduler_processes, &process->scheduler_node);
    ++scheduler_process_count;
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        if (thread->state != KERNEL_THREAD_NEW) return 0;
        thread->state = KERNEL_THREAD_RUNNABLE;
        if (!scheduler_enqueue(thread)) return 0;
        ++scheduler_thread_count;
    }
    return 1;
}

static void scheduler_destroy_processes(void)
{
    kernel_list_init(&scheduler_run_queue);
    kernel_list_init(&scheduler_reap_queue);
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&scheduler_processes)) != NULL) {
        kernel_process_destroy(scheduler_process_from_node(node));
    }
    scheduler_process_count = 0;
    scheduler_thread_count = 0;
}

static void scheduler_record_process_completion(KernelProcess *process)
{
    if (process == NULL || process->state != KERNEL_PROCESS_ZOMBIE ||
        process->live_thread_count != 0) {
        scheduler_completion_error = 1;
        return;
    }
    if (process->faulted) {
        ++scheduler_faulted_process_count;
        if (process->fault_cause != KERNEL_EXCEPTION_LOAD_PAGE_FAULT ||
            process->fault_address != 0 ||
            (process->fault_info & KERNEL_EINFO_ORIGIN_USER) == 0 ||
            process->exit_status !=
                -(int64_t)KERNEL_EXCEPTION_LOAD_PAGE_FAULT) {
            scheduler_completion_error = 1;
        }
    } else if (process->exit_status != 0) {
        scheduler_completion_error = 1;
    }
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        int valid_result = process->faulted
                               ? thread->exit_status == process->exit_status &&
                                     thread->write_count == 0
                               : thread->exit_status == 0 &&
                                     thread->write_count != 0;
        if (thread->state != KERNEL_THREAD_ZOMBIE || !valid_result ||
            thread->tid == 0 || thread->kernel_stack_top == 0) {
            scheduler_completion_error = 1;
        }
    }
    ++scheduler_completed_process_count;
    if (process->parent == NULL && !process->reap_queued) {
        kernel_list_push_back(&scheduler_reap_queue, &process->reap_node);
        process->reap_queued = 1;
    }
}

static void scheduler_reap_deferred(void)
{
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&scheduler_reap_queue)) != NULL) {
        KernelProcess *process = scheduler_reap_process_from_node(node);
        process->reap_queued = 0;
        if (scheduler_current != NULL &&
            scheduler_current->process == process) {
            kernel_list_push_back(&scheduler_reap_queue,
                                  &process->reap_node);
            process->reap_queued = 1;
            return;
        }
        kernel_list_remove(&scheduler_processes,
                           &process->scheduler_node);
        kernel_process_destroy(process);
        ++scheduler_reaped_process_count;
    }
}

static void scheduler_reschedule(uint64_t *frame, int timer_switch)
{
    KernelThread *previous = scheduler_current;
    scheduler_save(previous, frame);
    previous->state = KERNEL_THREAD_RUNNABLE;
    if (!scheduler_enqueue(previous)) return;
    KernelThread *next = scheduler_dequeue();
    if (next == NULL) return;
    if (timer_switch && next != previous) ++scheduler_preemption_count;
    scheduler_current = next;
    scheduler_load(previous, next, frame);
}

static void scheduler_finish(void)
{
    scheduler_active = 0;
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_CONTROL_OFFSET, 0);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                     IRQ_CONTROLLER_ENABLE_CLEAR_OFFSET,
                     UINT64_C(1) << KERNEL_TIMER_INTERRUPT_LINE);
    int heap_valid = kernel_heap_validate() == 0;
    int success = scheduler_timer_count != 0 &&
                  scheduler_preemption_count != 0 &&
                  scheduler_process_count == KERNEL_SCHEDULER_TEST_PROCESSES &&
                  scheduler_thread_count == KERNEL_SCHEDULER_TEST_THREADS &&
                  scheduler_completed_process_count ==
                      KERNEL_SCHEDULER_TEST_PROCESSES &&
                  scheduler_faulted_process_count == 1 &&
                  scheduler_reaped_process_count + 1 ==
                      scheduler_completed_process_count &&
                  scheduler_processes.count == 1 &&
                  scheduler_reap_queue.count == 1 &&
                  scheduler_completion_error == 0 && heap_valid;
    if (success) {
        kernel_uart_puts("KERNEL: USER FAULT ISOLATED\n");
        kernel_uart_puts("KERNEL: PROCESS REAP OK\n");
        kernel_uart_puts("KERNEL: PROCESS THREAD OK\n");
        kernel_uart_puts("KERNEL: USER SYSCALL OK\n");
        kernel_uart_puts("KERNEL: PREEMPTIVE SCHEDULER OK\n");
        kernel_uart_puts("KERNEL: READY\n");
    } else {
        kernel_uart_puts("KERNEL ERROR: scheduler runtime self-test\n");
        kernel_uart_puts("KERNEL DEBUG scheduler completed=");
        kernel_uart_put_hex64(scheduler_completed_process_count);
        kernel_uart_puts(" faulted=");
        kernel_uart_put_hex64(scheduler_faulted_process_count);
        kernel_uart_puts(" reaped=");
        kernel_uart_put_hex64(scheduler_reaped_process_count);
        kernel_uart_puts(" processes=");
        kernel_uart_put_hex64(scheduler_processes.count);
        kernel_uart_puts(" reapq=");
        kernel_uart_put_hex64(scheduler_reap_queue.count);
        kernel_uart_puts(" error=");
        kernel_uart_put_hex64((uint64_t)scheduler_completion_error);
        kernel_uart_puts(" heap=");
        kernel_uart_put_hex64((uint64_t)heap_valid);
        kernel_uart_puts("\n");
    }
    cvm_halt();
}

KernelAddressSpace *kernel_scheduler_current_space(void)
{
    if (!scheduler_active || scheduler_current == NULL ||
        scheduler_current->state != KERNEL_THREAD_RUNNING) {
        return NULL;
    }
    return scheduler_current->process->image.address_space;
}

uint64_t kernel_scheduler_current_pid(void)
{
    return kernel_scheduler_current_space() != NULL
               ? scheduler_current->process->pid : 0;
}

void kernel_scheduler_note_write(void)
{
    if (kernel_scheduler_current_space() != NULL) {
        ++scheduler_current->write_count;
    }
}

void kernel_scheduler_yield(uint64_t *frame)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    scheduler_reap_deferred();
    scheduler_reschedule(frame, 0);
}

void kernel_scheduler_exit(uint64_t *frame, int64_t status)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    scheduler_reap_deferred();
    KernelThread *previous = scheduler_current;
    scheduler_save(previous, frame);
    previous->exit_status = status;
    previous->state = KERNEL_THREAD_ZOMBIE;
    KernelProcess *process = previous->process;
    if (status != 0 && process->exit_status <= 0) {
        process->exit_status = status;
    }
    if (process->live_thread_count != 0) --process->live_thread_count;
    if (process->live_thread_count == 0) {
        if (process->exit_status == -1) process->exit_status = 0;
        process->state = KERNEL_PROCESS_ZOMBIE;
        scheduler_record_process_completion(process);
    }

    KernelThread *next = scheduler_dequeue();
    if (next == NULL) scheduler_finish();
    scheduler_current = next;
    scheduler_load(previous, next, frame);
}

void kernel_scheduler_fault(uint64_t *frame,
                            uint64_t cause,
                            uint64_t address,
                            uint64_t info)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    scheduler_reap_deferred();
    KernelThread *previous = scheduler_current;
    KernelProcess *process = previous->process;
    scheduler_save(previous, frame);
    int64_t status = -(int64_t)cause;
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        if (thread->queued) {
            kernel_list_remove(&scheduler_run_queue, &thread->run_node);
            thread->queued = 0;
        }
        if (thread->state != KERNEL_THREAD_DEAD) {
            thread->exit_status = status;
            thread->state = KERNEL_THREAD_ZOMBIE;
        }
    }
    process->live_thread_count = 0;
    process->exit_status = status;
    process->fault_cause = cause;
    process->fault_address = address;
    process->fault_info = info;
    process->faulted = 1;
    process->state = KERNEL_PROCESS_ZOMBIE;
    scheduler_record_process_completion(process);

    KernelThread *next = scheduler_dequeue();
    if (next == NULL) scheduler_finish();
    scheduler_current = next;
    scheduler_load(previous, next, frame);
}

void kernel_scheduler_timer(uint64_t *frame)
{
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_STATUS_OFFSET,
                     TIMER_STATUS_PENDING);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS + IRQ_CONTROLLER_EOI_OFFSET,
                     KERNEL_TIMER_INTERRUPT_LINE);
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    scheduler_reap_deferred();
    ++scheduler_timer_count;
    scheduler_reschedule(frame, 1);
}

static KernelProcess *scheduler_make_test_process(const char *message,
                                                  size_t thread_count)
{
    size_t image_size;
    uint8_t *data = kernel_user_test_program_create(
        message, 500000, &image_size);
    if (data == NULL) return NULL;
    KernelProcess *process = kernel_process_create(data, image_size);
    kernel_free(data);
    if (process == NULL) return NULL;
    for (size_t i = 0; i < thread_count; ++i) {
        if (kernel_thread_create(process) == NULL) {
            kernel_process_destroy(process);
            return NULL;
        }
    }
    return process;
}

static KernelProcess *scheduler_make_fault_process(size_t thread_count)
{
    size_t image_size;
    uint8_t *data = kernel_user_fault_test_program_create(&image_size);
    if (data == NULL) return NULL;
    KernelProcess *process = kernel_process_create(data, image_size);
    kernel_free(data);
    if (process == NULL) return NULL;
    for (size_t i = 0; i < thread_count; ++i) {
        if (kernel_thread_create(process) == NULL) {
            kernel_process_destroy(process);
            return NULL;
        }
    }
    return process;
}

static int scheduler_validate_test_layout(KernelProcess *first,
                                          KernelProcess *second,
                                          KernelProcess *third)
{
    if (first == NULL || second == NULL || third == NULL ||
        first->pid == 0 || second->pid == 0 || third->pid == 0 ||
        first->pid == second->pid || first->pid == third->pid ||
        second->pid == third->pid || first->thread_count != 2 ||
        second->thread_count != 1 || third->thread_count != 1) {
        return 0;
    }
    uintptr_t first_root = kernel_address_space_root(
        first->image.address_space);
    if (first_root == kernel_address_space_root(second->image.address_space) ||
        first_root == kernel_address_space_root(third->image.address_space) ||
        kernel_address_space_root(second->image.address_space) ==
            kernel_address_space_root(third->image.address_space)) {
        return 0;
    }
    KernelThread *thread_a = scheduler_process_thread_from_node(
        first->threads.sentinel.next);
    KernelThread *thread_b = scheduler_process_thread_from_node(
        first->threads.sentinel.next->next);
    return thread_a->process == thread_b->process &&
           thread_a->stack_pointer != thread_b->stack_pointer &&
           thread_a->kernel_stack_top != thread_b->kernel_stack_top;
}

int kernel_scheduler_self_test(void)
{
    scheduler_active = 0;
    scheduler_current = NULL;
    scheduler_process_count = 0;
    scheduler_thread_count = 0;
    scheduler_timer_count = 0;
    scheduler_preemption_count = 0;
    scheduler_completed_process_count = 0;
    scheduler_faulted_process_count = 0;
    scheduler_reaped_process_count = 0;
    scheduler_completion_error = 0;
    kernel_list_init(&scheduler_processes);
    kernel_list_init(&scheduler_run_queue);
    kernel_list_init(&scheduler_reap_queue);
    kernel_process_system_init();

    KernelProcess *first = scheduler_make_test_process(
        "USER[1]: shared process thread\n", 2);
    KernelProcess *second = scheduler_make_test_process(
        "USER[2]: syscall write\n", 1);
    KernelProcess *third = scheduler_make_fault_process(1);
    if (!scheduler_validate_test_layout(first, second, third)) {
        kernel_process_destroy(first);
        kernel_process_destroy(second);
        kernel_process_destroy(third);
        return 1;
    }
    if (!scheduler_register_process(first) ||
        !scheduler_register_process(second) ||
        !scheduler_register_process(third)) {
        scheduler_destroy_processes();
        return 1;
    }

    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                     IRQ_CONTROLLER_ENABLE_SET_OFFSET,
                     UINT64_C(1) << KERNEL_TIMER_INTERRUPT_LINE);
    if (cvm_mmio_read64((uintptr_t)KERNEL_IRQ_ALIAS +
                        IRQ_CONTROLLER_RESULT_OFFSET) !=
        IRQ_CONTROLLER_RESULT_SUCCESS) {
        scheduler_destroy_processes();
        return 1;
    }
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_COUNTER_OFFSET, 0);
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_COMPARE_OFFSET,
                     KERNEL_TIMER_QUANTUM);
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_STATUS_OFFSET,
                     TIMER_STATUS_PENDING);
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_CONTROL_OFFSET,
                     TIMER_CONTROL_ENABLE | TIMER_CONTROL_REPEAT |
                         TIMER_CONTROL_IRQ_ENABLE);
    scheduler_current = scheduler_dequeue();
    if (scheduler_current == NULL) {
        scheduler_destroy_processes();
        return 1;
    }
    scheduler_active = 1;
    kernel_uart_puts("KERNEL: USER TASKS START\n");
    kernel_start_user(
        kernel_address_space_root(
            scheduler_current->process->image.address_space),
        scheduler_current->pc,
        scheduler_current->stack_pointer,
        scheduler_current->kernel_stack_top);
    return 1;
}
