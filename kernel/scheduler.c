#include "kernel_internal.h"

#include "builtin_device_protocol.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

#define KERNEL_TIMER_QUANTUM_MILLISECONDS 2U
#define KERNEL_TIMER_QUANTUM_TICKS \
    (UINT64_C(2) * TIMER_TICKS_PER_MILLISECOND)
#define KERNEL_SCHEDULER_TEST_PROCESSES 2U
#define KERNEL_SCHEDULER_TEST_THREADS 4U
#define KERNEL_INIT_IMAGE_MAXIMUM ((size_t)4 * 1024 * 1024)
#define KERNEL_SECONDARY_DISPATCH_WAIT UINT64_C(2000000)

#define TRAP_REGISTER(frame, reg) ((frame)[15U - (reg)])
#define TRAP_PC(frame) ((frame)[16])
#define TRAP_FLAGS(frame) ((frame)[17])
#define TRAP_USER_SP(frame) ((frame)[18])

static KernelList scheduler_processes;
static KernelList scheduler_run_queue;
static KernelList scheduler_reap_queue;
static KernelList scheduler_thread_reap_queue;
static KernelList scheduler_timeout_queue;
static KernelWaitQueue scheduler_sleep_queue;
static KernelProcess *scheduler_init_process;
static size_t scheduler_process_count;
static size_t scheduler_thread_count;
static uint64_t scheduler_timer_count;
static uint64_t scheduler_preemption_count;
static size_t scheduler_completed_process_count;
static size_t scheduler_faulted_process_count;
static size_t scheduler_reaped_process_count;
static size_t scheduler_wait_block_count;
static size_t scheduler_wait_wake_count;
static size_t scheduler_wait_reap_count;
static size_t scheduler_join_block_count;
static size_t scheduler_join_wake_count;
static size_t scheduler_join_reap_count;
static size_t scheduler_exec_count;
static size_t scheduler_sleep_block_count;
static size_t scheduler_sleep_wake_count;
static size_t scheduler_idle_count;
static size_t scheduler_kernel_block_switch_count;
static int scheduler_completion_error;
static int scheduler_active;
static int scheduler_init_from_disk;
static size_t scheduler_secondary_dispatch_count;
static size_t scheduler_same_process_parallel_count;

#define scheduler_current (kernel_smp_current_thread())

static void scheduler_set_current(KernelThread *thread)
{
    kernel_smp_set_current_thread(thread);
}

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

static KernelThread *scheduler_reap_thread_from_node(KernelListNode *node)
{
    return (KernelThread *)((uint8_t *)node -
                            offsetof(KernelThread, reap_node));
}

static KernelThread *scheduler_wait_thread_from_node(KernelListNode *node)
{
    return (KernelThread *)((uint8_t *)node -
                            offsetof(KernelThread, wait_node));
}

static KernelThread *scheduler_timeout_thread_from_node(KernelListNode *node)
{
    return (KernelThread *)((uint8_t *)node -
                            offsetof(KernelThread, timeout_node));
}

static KernelProcess *scheduler_reap_process_from_node(KernelListNode *node)
{
    return (KernelProcess *)((uint8_t *)node -
                             offsetof(KernelProcess, reap_node));
}

static KernelProcess *scheduler_child_process_from_node(KernelListNode *node)
{
    return (KernelProcess *)((uint8_t *)node -
                             offsetof(KernelProcess, child_node));
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
                           KernelThread *thread,
                           uint64_t *frame)
{
    if (thread->kernel_resume_sp != 0) {
        uintptr_t resume_sp = thread->kernel_resume_sp;
        thread->kernel_resume_sp = 0;
        if (previous == NULL ||
            previous->process->image.address_space !=
                thread->process->image.address_space) {
            cvm_set_ptbr((uint64_t)kernel_address_space_root(
                thread->process->image.address_space));
        }
        cvm_set_ksp((uint64_t)thread->kernel_stack_top);
        kernel_smp_switch_release();
        kernel_resume_continuation(resume_sp);
        return;
    }
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
    uint64_t logical = kernel_smp_current_logical_id();
    uint64_t affinity = logical != UINT64_MAX ? logical + 1 : 1;
    size_t candidates = scheduler_run_queue.count;
    while (candidates-- != 0) {
        KernelListNode *node = kernel_list_pop_front(&scheduler_run_queue);
        if (node == NULL) return NULL;
        KernelThread *thread = scheduler_run_thread_from_node(node);
        if (thread->cpu_affinity != 0 &&
            thread->cpu_affinity != affinity) {
            kernel_list_push_back(&scheduler_run_queue, node);
            continue;
        }
        if (thread->cpu_affinity == 0) thread->cpu_affinity = affinity;
        if (kernel_smp_process_active(thread->process)) {
            ++scheduler_same_process_parallel_count;
        }
        thread->queued = 0;
        thread->state = KERNEL_THREAD_RUNNING;
        return thread;
    }
    return NULL;
}

static void scheduler_queue_reap(KernelProcess *process)
{
    if (process == NULL || process->parent != NULL || process->reap_queued) {
        return;
    }
    kernel_list_push_back(&scheduler_reap_queue, &process->reap_node);
    process->reap_queued = 1;
}

void kernel_wait_queue_init(KernelWaitQueue *queue)
{
    if (queue != NULL) kernel_list_init(&queue->waiters);
}

static void scheduler_detach_wait(KernelThread *thread)
{
    if (thread == NULL) return;
    if (thread->wait_queue != NULL) {
        kernel_list_remove(&thread->wait_queue->waiters, &thread->wait_node);
        thread->wait_queue = NULL;
    }
    if (thread->timeout_queued) {
        kernel_list_remove(&scheduler_timeout_queue, &thread->timeout_node);
        thread->timeout_queued = 0;
    }
    thread->wait_deadline = 0;
    thread->wait_timeout_result = 0;
    thread->wait_buffer_address = 0;
    thread->wait_buffer_size = 0;
}

static int scheduler_wait_matches(uint64_t requested_pid,
                                  const KernelProcess *child)
{
    return child != NULL &&
           (requested_pid == 0 || requested_pid == UINT64_MAX ||
            requested_pid == child->pid);
}

static void scheduler_wake_waiter(KernelThread *thread, int64_t result)
{
    if (thread == NULL || thread->state != KERNEL_THREAD_BLOCKED) return;
    scheduler_detach_wait(thread);
    thread->registers[0] = (uint64_t)result;
    thread->kernel_wait_result = result;
    thread->wait_pid = 0;
    thread->wait_status_address = 0;
    thread->wait_tid = 0;
    thread->join_status_address = 0;
    thread->state = KERNEL_THREAD_RUNNABLE;
    if (!scheduler_enqueue(thread)) {
        scheduler_completion_error = 1;
    } else if (scheduler_active) {
        if (thread->cpu_affinity != 0) {
            (void)kernel_smp_wake_logical(thread->cpu_affinity - 1);
        } else {
            (void)kernel_smp_wake_idle_other();
        }
    }
}

size_t kernel_wait_queue_wake_one(KernelWaitQueue *queue, int64_t result)
{
    if (queue == NULL || kernel_list_empty(&queue->waiters)) return 0;
    KernelThread *thread = scheduler_wait_thread_from_node(
        queue->waiters.sentinel.next);
    if (thread->state != KERNEL_THREAD_BLOCKED ||
        thread->wait_queue != queue) {
        scheduler_completion_error = 1;
        return 0;
    }
    scheduler_wake_waiter(thread, result);
    return 1;
}

size_t kernel_wait_queue_wake_all(KernelWaitQueue *queue, int64_t result)
{
    if (queue == NULL) return 0;
    size_t count = 0;
    while (!kernel_list_empty(&queue->waiters)) {
        if (kernel_wait_queue_wake_one(queue, result) != 1) {
            scheduler_completion_error = 1;
            break;
        }
        ++count;
    }
    return count;
}

size_t kernel_wait_queue_wake_read_one(KernelWaitQueue *queue,
                                       const void *data,
                                       size_t size)
{
    if (queue == NULL || data == NULL || size == 0 ||
        kernel_list_empty(&queue->waiters)) {
        return 0;
    }
    KernelThread *thread = scheduler_wait_thread_from_node(
        queue->waiters.sentinel.next);
    if (thread->state != KERNEL_THREAD_BLOCKED ||
        thread->wait_queue != queue || thread->wait_buffer_size == 0) {
        scheduler_completion_error = 1;
        return 0;
    }
    size_t copied = size < thread->wait_buffer_size
                        ? size : thread->wait_buffer_size;
    int64_t result = kernel_copy_to_user(
                         thread->process->image.address_space,
                         thread->wait_buffer_address,
                         data,
                         copied)
                         ? (int64_t)copied
                         : -(int64_t)KERNEL_ERROR_FAULT;
    scheduler_wake_waiter(thread, result);
    return 1;
}

static void scheduler_queue_thread_reap(KernelThread *thread)
{
    if (thread == NULL || thread->state != KERNEL_THREAD_ZOMBIE ||
        thread->reap_queued) {
        scheduler_completion_error = 1;
        return;
    }
    kernel_list_push_back(&scheduler_thread_reap_queue, &thread->reap_node);
    thread->reap_queued = 1;
    ++scheduler_join_reap_count;
}

static KernelThread *scheduler_find_thread(KernelProcess *process, uint64_t tid)
{
    if (process == NULL || tid == 0) return NULL;
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        if (thread->tid == tid) return thread;
        node = node->next;
    }
    return NULL;
}

static void scheduler_notify_joiner(KernelThread *target)
{
    KernelThread *waiter = target != NULL ? target->join_waiter : NULL;
    if (waiter == NULL) return;
    target->join_waiter = NULL;
    if (waiter->state != KERNEL_THREAD_BLOCKED ||
        waiter->wait_tid != target->tid) {
        scheduler_completion_error = 1;
        return;
    }
    if (waiter->join_status_address != 0 &&
        !kernel_copy_to_user(waiter->process->image.address_space,
                             waiter->join_status_address,
                             &target->exit_status,
                             sizeof(target->exit_status))) {
        scheduler_wake_waiter(waiter, -KERNEL_ERROR_FAULT);
        return;
    }
    uint64_t tid = target->tid;
    scheduler_queue_thread_reap(target);
    scheduler_wake_waiter(waiter, (int64_t)tid);
    ++scheduler_join_wake_count;
}

static void scheduler_collect_child(KernelProcess *parent,
                                    KernelProcess *child)
{
    if (parent == NULL || child == NULL || child->parent != parent) return;
    kernel_list_remove(&parent->children, &child->child_node);
    child->parent = NULL;
    ++scheduler_wait_reap_count;
    scheduler_queue_reap(child);
}

static void scheduler_wake_stale_waiters(KernelProcess *parent,
                                         uint64_t reaped_pid)
{
    if (parent == NULL) return;
    KernelListNode *node = parent->threads.sentinel.next;
    while (node != &parent->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        if (thread->state != KERNEL_THREAD_BLOCKED) continue;
        int waits_for_reaped_pid = thread->wait_pid == reaped_pid;
        int waits_for_any_child = thread->wait_pid == 0 ||
                                  thread->wait_pid == UINT64_MAX;
        if (waits_for_reaped_pid ||
            (waits_for_any_child && parent->children.count == 0)) {
            scheduler_wake_waiter(thread, -KERNEL_ERROR_NO_CHILD);
        }
    }
}

static void scheduler_notify_parent(KernelProcess *child)
{
    KernelProcess *parent = child != NULL ? child->parent : NULL;
    if (parent == NULL) return;
    KernelListNode *node = parent->threads.sentinel.next;
    while (node != &parent->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        if (thread->state != KERNEL_THREAD_BLOCKED ||
            !scheduler_wait_matches(thread->wait_pid, child)) {
            continue;
        }
        if (thread->wait_status_address != 0 &&
            !kernel_copy_to_user(parent->image.address_space,
                                 thread->wait_status_address,
                                 &child->exit_status,
                                 sizeof(child->exit_status))) {
            scheduler_wake_waiter(thread, -KERNEL_ERROR_FAULT);
            continue;
        }
        uint64_t child_pid = child->pid;
        scheduler_collect_child(parent, child);
        scheduler_wake_waiter(thread, (int64_t)child_pid);
        scheduler_wake_stale_waiters(parent, child_pid);
        ++scheduler_wait_wake_count;
        return;
    }
}

static void scheduler_orphan_children(KernelProcess *parent)
{
    if (parent == NULL) return;
    KernelProcess *new_parent =
        parent != scheduler_init_process && scheduler_init_process != NULL &&
                scheduler_init_process->state != KERNEL_PROCESS_ZOMBIE
            ? scheduler_init_process
            : NULL;
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&parent->children)) != NULL) {
        KernelProcess *child = scheduler_child_process_from_node(node);
        child->parent = new_parent;
        if (new_parent != NULL) {
            kernel_list_push_back(&new_parent->children, &child->child_node);
        } else if (child->state == KERNEL_PROCESS_ZOMBIE) {
            scheduler_queue_reap(child);
        }
    }
}

int kernel_scheduler_register_process(KernelProcess *process)
{
    if (process == NULL || process->thread_count == 0 ||
        process->registered || process->reap_queued) {
        return 0;
    }
    size_t validated = 0;
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        if (thread->process != process || thread->state != KERNEL_THREAD_NEW ||
            thread->queued || thread->reap_queued || thread->tid == 0) {
            return 0;
        }
        ++validated;
        node = node->next;
    }
    if (validated != process->thread_count ||
        process->live_thread_count != process->thread_count) {
        return 0;
    }

    kernel_list_push_back(&scheduler_processes, &process->scheduler_node);
    process->registered = 1;
    ++scheduler_process_count;
    size_t enqueued = 0;
    node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        thread->state = KERNEL_THREAD_RUNNABLE;
        if (!scheduler_enqueue(thread)) {
            KernelListNode *rollback = process->threads.sentinel.next;
            while (rollback != &process->threads.sentinel) {
                KernelThread *queued = scheduler_process_thread_from_node(rollback);
                rollback = rollback->next;
                if (queued->queued) {
                    kernel_list_remove(&scheduler_run_queue, &queued->run_node);
                    queued->queued = 0;
                }
                queued->state = KERNEL_THREAD_NEW;
            }
            kernel_list_remove(&scheduler_processes, &process->scheduler_node);
            process->registered = 0;
            --scheduler_process_count;
            scheduler_thread_count -= enqueued;
            return 0;
        }
        ++scheduler_thread_count;
        ++enqueued;
    }
    return 1;
}

static void scheduler_destroy_processes(void)
{
    kernel_list_init(&scheduler_run_queue);
    kernel_list_init(&scheduler_reap_queue);
    KernelListNode *thread_node;
    while ((thread_node = kernel_list_pop_front(
                &scheduler_thread_reap_queue)) != NULL) {
        KernelThread *thread = scheduler_reap_thread_from_node(thread_node);
        thread->reap_queued = 0;
        (void)kernel_thread_destroy(thread);
    }
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&scheduler_processes)) != NULL) {
        KernelProcess *process = scheduler_process_from_node(node);
        KernelListNode *thread_node = process->threads.sentinel.next;
        while (thread_node != &process->threads.sentinel) {
            KernelThread *thread = scheduler_process_thread_from_node(
                thread_node);
            thread_node = thread_node->next;
            scheduler_detach_wait(thread);
        }
        if (process == scheduler_init_process) scheduler_init_process = NULL;
        process->registered = 0;
        kernel_process_destroy(process);
    }
    scheduler_process_count = 0;
    scheduler_thread_count = 0;
    kernel_list_init(&scheduler_timeout_queue);
    kernel_wait_queue_init(&scheduler_sleep_queue);
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
                               ? thread->exit_status == process->exit_status
                               : thread->exit_status == 0 &&
                                     thread->write_count != 0;
        if (thread->state != KERNEL_THREAD_ZOMBIE || !valid_result ||
            thread->tid == 0 || thread->kernel_stack_top == 0) {
            scheduler_completion_error = 1;
        }
    }
    scheduler_orphan_children(process);
    ++scheduler_completed_process_count;
    scheduler_notify_parent(process);
    scheduler_queue_reap(process);
}

static void scheduler_reap_deferred(void)
{
    KernelListNode *thread_node;
    while ((thread_node = kernel_list_pop_front(
                &scheduler_thread_reap_queue)) != NULL) {
        KernelThread *thread = scheduler_reap_thread_from_node(thread_node);
        thread->reap_queued = 0;
        if (kernel_smp_thread_active(thread)) {
            kernel_list_push_back(&scheduler_thread_reap_queue,
                                  &thread->reap_node);
            thread->reap_queued = 1;
            return;
        }
        if (kernel_thread_destroy(thread) != 0) {
            scheduler_completion_error = 1;
        }
    }
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&scheduler_reap_queue)) != NULL) {
        KernelProcess *process = scheduler_reap_process_from_node(node);
        process->reap_queued = 0;
        if (kernel_smp_process_active(process)) {
            kernel_list_push_back(&scheduler_reap_queue,
                                  &process->reap_node);
            process->reap_queued = 1;
            return;
        }
        kernel_list_remove(&scheduler_processes,
                           &process->scheduler_node);
        if (process == scheduler_init_process) scheduler_init_process = NULL;
        process->registered = 0;
        kernel_process_destroy(process);
        ++scheduler_reaped_process_count;
    }
}

static int scheduler_has_blocked_thread(void)
{
    KernelListNode *process_node = scheduler_processes.sentinel.next;
    while (process_node != &scheduler_processes.sentinel) {
        KernelProcess *process = scheduler_process_from_node(process_node);
        process_node = process_node->next;
        KernelListNode *thread_node = process->threads.sentinel.next;
        while (thread_node != &process->threads.sentinel) {
            KernelThread *thread = scheduler_process_thread_from_node(
                thread_node);
            if (thread->state == KERNEL_THREAD_BLOCKED) return 1;
            thread_node = thread_node->next;
        }
    }
    return 0;
}

static KernelThread *scheduler_dequeue_or_idle(void)
{
    KernelThread *next;
    while ((next = scheduler_dequeue()) == NULL) {
        if (!scheduler_has_blocked_thread() &&
            !kernel_smp_has_running_thread()) {
            return NULL;
        }
        scheduler_set_current(NULL);
        ++scheduler_idle_count;
        kernel_smp_idle_enter();
        kernel_smp_switch_release();
        kernel_idle_wait();
        kernel_smp_switch_reacquire();
        kernel_smp_idle_leave();
        scheduler_reap_deferred();
    }
    return next;
}

static void scheduler_expire_timeouts(void)
{
    KernelListNode *node = scheduler_timeout_queue.sentinel.next;
    while (node != &scheduler_timeout_queue.sentinel) {
        KernelThread *thread = scheduler_timeout_thread_from_node(node);
        node = node->next;
        if (thread->timeout_queued &&
            thread->wait_deadline <= scheduler_timer_count) {
            int sleeping = thread->wait_queue == &scheduler_sleep_queue;
            int64_t result = thread->wait_timeout_result;
            scheduler_wake_waiter(thread, result);
            if (sleeping) ++scheduler_sleep_wake_count;
        }
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
    scheduler_set_current(next);
    scheduler_load(previous, next, frame);
}

static void scheduler_finish(void)
{
    scheduler_active = 0;
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_CONTROL_OFFSET, 0);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                     IRQ_CONTROLLER_ENABLE_CLEAR_OFFSET,
                     UINT64_C(1) << KERNEL_TIMER_INTERRUPT_LINE);
    scheduler_reap_deferred();
    uint64_t smp_logical = kernel_smp_logical_count();
    int smp_valid = kernel_smp_runtime_valid();
    int smp_stopped = kernel_smp_shutdown_secondary() == 0;
    int heap_valid = kernel_heap_validate() == 0;
    int success = scheduler_timer_count != 0 &&
                   (scheduler_preemption_count != 0 ||
                    scheduler_secondary_dispatch_count != 0) &&
                  scheduler_init_from_disk &&
                  scheduler_process_count == KERNEL_SCHEDULER_TEST_PROCESSES &&
                  scheduler_thread_count == KERNEL_SCHEDULER_TEST_THREADS &&
                  scheduler_completed_process_count ==
                      KERNEL_SCHEDULER_TEST_PROCESSES &&
                  scheduler_faulted_process_count == 1 &&
                  ((scheduler_wait_block_count == 1 &&
                    scheduler_wait_wake_count == 1) ||
                   (smp_logical > 1 && scheduler_wait_block_count == 0 &&
                    scheduler_wait_wake_count == 0)) &&
                  scheduler_wait_reap_count == 1 &&
                  ((scheduler_join_block_count == 1 &&
                    scheduler_join_wake_count == 1) ||
                   (smp_logical > 1 && scheduler_join_block_count == 0 &&
                    scheduler_join_wake_count == 0)) &&
                  scheduler_join_reap_count == 1 &&
                  scheduler_exec_count == 1 &&
                  scheduler_sleep_block_count == 1 &&
                   scheduler_sleep_wake_count == 1 &&
                   scheduler_idle_count != 0 &&
                   (scheduler_kernel_block_switch_count != 0 ||
                    scheduler_secondary_dispatch_count != 0) &&
                  scheduler_timeout_queue.count == 0 &&
                  scheduler_sleep_queue.waiters.count == 0 &&
                  kernel_devices_runtime_valid() &&
                  scheduler_reaped_process_count + 1 ==
                      scheduler_completed_process_count &&
                  scheduler_processes.count == 1 &&
                  scheduler_reap_queue.count == 1 &&
                   scheduler_completion_error == 0 && heap_valid &&
                   smp_valid && smp_stopped &&
                   (smp_logical == 1 ||
                    (scheduler_secondary_dispatch_count != 0 &&
                     scheduler_same_process_parallel_count != 0));
    if (success) {
        kernel_uart_puts("KERNEL: /BIN/INIT.EXF PID 1 OK\n");
        kernel_uart_puts("KERNEL: USER FAULT ISOLATED\n");
        kernel_uart_puts("KERNEL: PROCESS REAP OK\n");
        kernel_uart_puts("KERNEL: WAITPID BLOCK/WAKE OK\n");
        kernel_uart_puts("KERNEL: THREAD JOIN OK\n");
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
        kernel_uart_puts(" wait=");
        kernel_uart_put_hex64(scheduler_wait_block_count);
        kernel_uart_puts("/");
        kernel_uart_put_hex64(scheduler_wait_wake_count);
        kernel_uart_puts("/");
        kernel_uart_put_hex64(scheduler_wait_reap_count);
        kernel_uart_puts(" join=");
        kernel_uart_put_hex64(scheduler_join_block_count);
        kernel_uart_puts("/");
        kernel_uart_put_hex64(scheduler_join_wake_count);
        kernel_uart_puts("/");
        kernel_uart_put_hex64(scheduler_join_reap_count);
        kernel_uart_puts(" exec=");
        kernel_uart_put_hex64(scheduler_exec_count);
        kernel_uart_puts(" sleep=");
        kernel_uart_put_hex64(scheduler_sleep_block_count);
        kernel_uart_puts("/");
        kernel_uart_put_hex64(scheduler_sleep_wake_count);
        kernel_uart_puts(" idle=");
        kernel_uart_put_hex64(scheduler_idle_count);
        kernel_uart_puts(" timeoutq=");
        kernel_uart_put_hex64(scheduler_timeout_queue.count);
        kernel_uart_puts(" processes=");
        kernel_uart_put_hex64(scheduler_processes.count);
        kernel_uart_puts(" reapq=");
        kernel_uart_put_hex64(scheduler_reap_queue.count);
        kernel_uart_puts(" error=");
        kernel_uart_put_hex64((uint64_t)scheduler_completion_error);
        kernel_uart_puts(" heap=");
        kernel_uart_put_hex64((uint64_t)heap_valid);
        kernel_uart_puts(" smp=");
        kernel_uart_put_hex64((uint64_t)smp_valid);
        kernel_uart_puts("/");
        kernel_uart_put_hex64((uint64_t)smp_stopped);
        kernel_uart_puts(" dispatch=");
        kernel_uart_put_hex64(scheduler_secondary_dispatch_count);
        kernel_uart_puts(" parallel=");
        kernel_uart_put_hex64(scheduler_same_process_parallel_count);
        kernel_uart_puts("\n");
    }
    cvm_halt();
}

static void scheduler_complete_or_wait(void)
{
    scheduler_set_current(NULL);
    if (kernel_smp_is_boot_cpu()) scheduler_finish();
    kernel_smp_secondary_retire();
}

static int scheduler_prepare_retire_frame(uint64_t *frame)
{
    if (frame == NULL) return 0;
    TRAP_PC(frame) = (uint64_t)(uintptr_t)kernel_secondary_retire_entry;
    TRAP_FLAGS(frame) &= KERNEL_SAVED_EXCEPTION_FRAME;
    return 1;
}

static int scheduler_retire_secondary_frame(uint64_t *frame)
{
    if (kernel_smp_is_boot_cpu()) return 0;
    scheduler_set_current(NULL);
    return scheduler_prepare_retire_frame(frame);
}

KernelAddressSpace *kernel_scheduler_current_space(void)
{
    if (!scheduler_active || scheduler_current == NULL ||
        scheduler_current->state != KERNEL_THREAD_RUNNING) {
        return NULL;
    }
    return scheduler_current->process->image.address_space;
}

KernelProcess *kernel_scheduler_current_process(void)
{
    return kernel_scheduler_current_space() != NULL
               ? scheduler_current->process : NULL;
}

KernelFdTable *kernel_scheduler_current_fd_table(void)
{
    return kernel_scheduler_current_space() != NULL
               ? scheduler_current->process->fd_table : NULL;
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

int kernel_scheduler_block_current(uint64_t *frame,
                                   KernelWaitQueue *queue,
                                   uint64_t timeout_ticks,
                                   int64_t timeout_result)
{
    if (frame == NULL || queue == NULL ||
        (timeout_ticks != 0 &&
         timeout_ticks > UINT64_MAX - scheduler_timer_count) ||
        kernel_scheduler_current_space() == NULL) {
        return 1;
    }
    KernelThread *waiter = scheduler_current;
    if (waiter->wait_queue != NULL || waiter->timeout_queued) return 1;

    scheduler_save(waiter, frame);
    waiter->state = KERNEL_THREAD_BLOCKED;
    waiter->wait_queue = queue;
    kernel_list_push_back(&queue->waiters, &waiter->wait_node);
    if (timeout_ticks != 0) {
        waiter->wait_deadline = scheduler_timer_count + timeout_ticks;
        waiter->wait_timeout_result = timeout_result;
        kernel_list_push_back(&scheduler_timeout_queue,
                              &waiter->timeout_node);
        waiter->timeout_queued = 1;
    }

    KernelThread *next = scheduler_dequeue_or_idle();
    if (next == NULL) {
        scheduler_set_current(waiter);
        scheduler_detach_wait(waiter);
        waiter->state = KERNEL_THREAD_RUNNING;
        return 1;
    }
    scheduler_set_current(next);
    scheduler_load(waiter, next, frame);
    return 0;
}

int kernel_scheduler_block_read(uint64_t *frame,
                                KernelWaitQueue *queue,
                                uintptr_t buffer_address,
                                size_t buffer_size)
{
    if (frame == NULL || queue == NULL || buffer_size == 0 ||
        kernel_scheduler_current_space() == NULL) {
        return 1;
    }
    KernelThread *waiter = scheduler_current;
    waiter->wait_buffer_address = buffer_address;
    waiter->wait_buffer_size = buffer_size;
    if (kernel_scheduler_block_current(frame, queue, 0, 0) != 0) {
        waiter->wait_buffer_address = 0;
        waiter->wait_buffer_size = 0;
        return 1;
    }
    return 0;
}

int kernel_scheduler_block_kernel(KernelWaitQueue *queue,
                                  uint64_t timeout_ticks,
                                  int64_t timeout_result,
                                  int64_t *wake_result)
{
    KernelThread *waiter = scheduler_current;
    if (queue == NULL || wake_result == NULL || waiter == NULL ||
        waiter->active_trap_frame == NULL || waiter->kernel_resume_sp != 0 ||
        waiter->wait_queue != NULL || waiter->timeout_queued ||
        (timeout_ticks != 0 &&
         timeout_ticks > UINT64_MAX - scheduler_timer_count)) {
        return 1;
    }

    scheduler_save(waiter, waiter->active_trap_frame);
    waiter->state = KERNEL_THREAD_BLOCKED;
    waiter->wait_queue = queue;
    waiter->kernel_wait_result = timeout_result;
    kernel_list_push_back(&queue->waiters, &waiter->wait_node);
    if (timeout_ticks != 0) {
        waiter->wait_deadline = scheduler_timer_count + timeout_ticks;
        waiter->wait_timeout_result = timeout_result;
        kernel_list_push_back(&scheduler_timeout_queue,
                              &waiter->timeout_node);
        waiter->timeout_queued = 1;
    }

    KernelThread *next = scheduler_dequeue_or_idle();
    if (next == NULL) {
        scheduler_set_current(waiter);
        scheduler_detach_wait(waiter);
        waiter->state = KERNEL_THREAD_RUNNING;
        return 1;
    }
    scheduler_set_current(next);
    if (next != waiter) {
        ++scheduler_kernel_block_switch_count;
        if (next->kernel_resume_sp != 0) {
            uintptr_t next_resume_sp = next->kernel_resume_sp;
            next->kernel_resume_sp = 0;
            if (waiter->process->image.address_space !=
                next->process->image.address_space) {
                cvm_set_ptbr((uint64_t)kernel_address_space_root(
                    next->process->image.address_space));
            }
            cvm_set_ksp((uint64_t)next->kernel_stack_top);
            kernel_smp_switch_release();
            kernel_switch_continuation(&waiter->kernel_resume_sp,
                                       next_resume_sp);
            kernel_smp_switch_reacquire();
        } else {
            kernel_smp_switch_release();
            kernel_suspend_to_user(
                &waiter->kernel_resume_sp,
                kernel_address_space_root(next->process->image.address_space),
                next->pc,
                next->stack_pointer,
                next->kernel_stack_top,
                next->registers);
            kernel_smp_switch_reacquire();
        }
    }
    *wake_result = waiter->kernel_wait_result;
    waiter->kernel_wait_result = 0;
    return 0;
}

void kernel_scheduler_syscall_enter(uint64_t *frame)
{
    if (scheduler_current != NULL && frame != NULL) {
        scheduler_current->active_trap_frame = frame;
    }
}

void kernel_scheduler_syscall_leave(uint64_t *frame)
{
    if (scheduler_current != NULL &&
        scheduler_current->active_trap_frame == frame) {
        scheduler_current->active_trap_frame = NULL;
    }
}

int kernel_scheduler_termination_requested(int64_t *status)
{
    if (scheduler_current == NULL || scheduler_current->process == NULL ||
        !scheduler_current->process->faulted) {
        return 0;
    }
    if (status != NULL) *status = scheduler_current->process->exit_status;
    return 1;
}

int kernel_scheduler_running(void)
{
    return scheduler_active;
}

uint64_t kernel_scheduler_ticks(void)
{
    return scheduler_timer_count;
}

void kernel_scheduler_interrupt_return(uint64_t *frame)
{
    if (frame != NULL &&
        TRAP_PC(frame) == (uint64_t)(uintptr_t)kernel_idle_wait_instruction) {
        TRAP_PC(frame) = (uint64_t)(uintptr_t)kernel_idle_wait_resume;
    }
}

int kernel_scheduler_sleep(uint64_t *frame,
                           uint64_t milliseconds,
                           int64_t *result)
{
    if (result == NULL) return 1;
    if (frame == NULL || kernel_scheduler_current_space() == NULL) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }
    if (milliseconds == 0) {
        *result = 0;
        return 1;
    }
    if (milliseconds >
        UINT64_MAX - (KERNEL_TIMER_QUANTUM_MILLISECONDS - 1U)) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }
    uint64_t ticks =
        (milliseconds + KERNEL_TIMER_QUANTUM_MILLISECONDS - 1U) /
        KERNEL_TIMER_QUANTUM_MILLISECONDS;
    if (ticks == 0 || ticks > UINT64_MAX - scheduler_timer_count) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }
    scheduler_reap_deferred();
    ++scheduler_sleep_block_count;
    if (kernel_scheduler_block_current(frame, &scheduler_sleep_queue,
                                       ticks, 0) != 0) {
        --scheduler_sleep_block_count;
        *result = -KERNEL_ERROR_AGAIN;
        return 1;
    }
    return 0;
}

int kernel_scheduler_waitpid(uint64_t *frame,
                             uint64_t pid,
                             uintptr_t status_address,
                             int64_t *result)
{
    if (result == NULL) return 1;
    if (frame == NULL || kernel_scheduler_current_space() == NULL ||
        (pid != 0 && pid != UINT64_MAX && pid > INT64_MAX)) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }

    scheduler_reap_deferred();
    KernelThread *waiter = scheduler_current;
    KernelProcess *parent = waiter->process;
    KernelProcess *zombie = NULL;
    int matching_child = 0;
    KernelListNode *node = parent->children.sentinel.next;
    while (node != &parent->children.sentinel) {
        KernelProcess *child = scheduler_child_process_from_node(node);
        node = node->next;
        if (!scheduler_wait_matches(pid, child)) continue;
        matching_child = 1;
        if (child->state == KERNEL_PROCESS_ZOMBIE) {
            zombie = child;
            break;
        }
    }

    if (!matching_child) {
        *result = -KERNEL_ERROR_NO_CHILD;
        return 1;
    }
    if (zombie != NULL) {
        if (status_address != 0 &&
            !kernel_copy_to_user(parent->image.address_space,
                                 status_address,
                                 &zombie->exit_status,
                                 sizeof(zombie->exit_status))) {
            *result = -KERNEL_ERROR_FAULT;
            return 1;
        }
        uint64_t child_pid = zombie->pid;
        scheduler_collect_child(parent, zombie);
        scheduler_wake_stale_waiters(parent, child_pid);
        *result = (int64_t)child_pid;
        return 1;
    }

    scheduler_save(waiter, frame);
    waiter->wait_pid = pid;
    waiter->wait_status_address = status_address;
    waiter->state = KERNEL_THREAD_BLOCKED;
    ++scheduler_wait_block_count;
    KernelThread *next = scheduler_dequeue_or_idle();
    if (next == NULL) {
        scheduler_set_current(waiter);
        waiter->wait_pid = 0;
        waiter->wait_status_address = 0;
        waiter->state = KERNEL_THREAD_RUNNING;
        --scheduler_wait_block_count;
        *result = -KERNEL_ERROR_AGAIN;
        return 1;
    }
    scheduler_set_current(next);
    scheduler_load(waiter, next, frame);
    return 0;
}

int kernel_scheduler_join(uint64_t *frame,
                          uint64_t tid,
                          uintptr_t status_address,
                          int64_t *result)
{
    if (result == NULL) return 1;
    if (frame == NULL || kernel_scheduler_current_space() == NULL || tid == 0 ||
        tid > INT64_MAX) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }

    scheduler_reap_deferred();
    KernelThread *waiter = scheduler_current;
    if (waiter->tid == tid) {
        *result = -KERNEL_ERROR_DEADLOCK;
        return 1;
    }
    KernelThread *target = scheduler_find_thread(waiter->process, tid);
    if (target == NULL || target->state == KERNEL_THREAD_DEAD ||
        target->reap_queued) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }
    if (target->join_waiter != NULL) {
        *result = -KERNEL_ERROR_BUSY;
        return 1;
    }
    if (target->state == KERNEL_THREAD_ZOMBIE) {
        if (status_address != 0 &&
            !kernel_copy_to_user(waiter->process->image.address_space,
                                 status_address,
                                 &target->exit_status,
                                 sizeof(target->exit_status))) {
            *result = -KERNEL_ERROR_FAULT;
            return 1;
        }
        uint64_t joined_tid = target->tid;
        scheduler_queue_thread_reap(target);
        *result = (int64_t)joined_tid;
        return 1;
    }

    scheduler_save(waiter, frame);
    waiter->wait_tid = tid;
    waiter->join_status_address = status_address;
    waiter->state = KERNEL_THREAD_BLOCKED;
    target->join_waiter = waiter;
    ++scheduler_join_block_count;
    KernelThread *next = scheduler_dequeue_or_idle();
    if (next == NULL) {
        scheduler_set_current(waiter);
        target->join_waiter = NULL;
        waiter->wait_tid = 0;
        waiter->join_status_address = 0;
        waiter->state = KERNEL_THREAD_RUNNING;
        --scheduler_join_block_count;
        *result = -KERNEL_ERROR_AGAIN;
        return 1;
    }
    scheduler_set_current(next);
    scheduler_load(waiter, next, frame);
    return 0;
}

static void scheduler_exec_discard_other_threads(KernelProcess *process,
                                                  KernelThread *current)
{
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        if (thread == current) continue;
        if (thread->queued) {
            kernel_list_remove(&scheduler_run_queue, &thread->run_node);
            thread->queued = 0;
        }
        if (thread->reap_queued) {
            kernel_list_remove(&scheduler_thread_reap_queue,
                               &thread->reap_node);
            thread->reap_queued = 0;
        }
        scheduler_detach_wait(thread);
        if (thread->wait_tid != 0) {
            KernelThread *target = scheduler_find_thread(
                process, thread->wait_tid);
            if (target != NULL && target->join_waiter == thread) {
                target->join_waiter = NULL;
            }
        }
        thread->join_waiter = NULL;
        kernel_list_remove(&process->threads, &thread->process_node);
        thread->state = KERNEL_THREAD_DEAD;
        thread->process = NULL;
        kernel_free(thread->kernel_stack);
        kernel_free(thread);
    }
    process->thread_count = 1;
    process->live_thread_count = 1;
}

static void scheduler_request_process_stop(KernelProcess *process,
                                           KernelThread *owner)
{
    process->stop_owner = owner;
    process->stop_requested = 1;
    KernelListNode *node = process->threads.sentinel.next;
    while (node != &process->threads.sentinel) {
        KernelThread *thread = scheduler_process_thread_from_node(node);
        node = node->next;
        if (thread == owner) continue;
        if (thread->queued) {
            kernel_list_remove(&scheduler_run_queue, &thread->run_node);
            thread->queued = 0;
        }
        if (thread->reap_queued) {
            kernel_list_remove(&scheduler_thread_reap_queue,
                               &thread->reap_node);
            thread->reap_queued = 0;
        }
        scheduler_detach_wait(thread);
        if (!kernel_smp_thread_active(thread) &&
            thread->state != KERNEL_THREAD_DEAD) {
            thread->state = KERNEL_THREAD_ZOMBIE;
        }
    }
}

int kernel_scheduler_stop_current(uint64_t *frame)
{
    KernelThread *thread = scheduler_current;
    if (frame == NULL || thread == NULL || thread->process == NULL ||
        !thread->process->stop_requested ||
        thread->process->stop_owner == thread) {
        return 0;
    }
    scheduler_save(thread, frame);
    scheduler_detach_wait(thread);
    thread->exit_status = thread->process->faulted
                              ? thread->process->exit_status : 0;
    thread->state = KERNEL_THREAD_ZOMBIE;
    return scheduler_prepare_retire_frame(frame);
}

static void scheduler_copy_user_image(KernelUserImage *destination,
                                      const KernelUserImage *source)
{
    destination->address_space = source->address_space;
    destination->entry = source->entry;
    destination->stack_pointer = source->stack_pointer;
    destination->image_base = source->image_base;
    destination->image_end = source->image_end;
}

int kernel_scheduler_exec(uint64_t *frame,
                          const char *path,
                          size_t argument_count,
                          const char *const *arguments,
                          size_t environment_count,
                          const char *const *environment,
                          int64_t *result)
{
    if (result == NULL) return 1;
    if (frame == NULL || path == NULL ||
        kernel_scheduler_current_space() == NULL) {
        *result = -KERNEL_ERROR_INVALID;
        return 1;
    }

    scheduler_reap_deferred();
    KernelUserImage new_image;
    if (kernel_user_image_load_path(path, KERNEL_INIT_IMAGE_MAXIMUM,
                                    &new_image) != 0) {
        *result = -KERNEL_ERROR_EXEC_FORMAT;
        return 1;
    }

    KernelProcess prepared_process;
    scheduler_copy_user_image(&prepared_process.image, &new_image);
    KernelThread prepared_thread;
    prepared_thread.process = &prepared_process;
    prepared_thread.state = KERNEL_THREAD_NEW;
    prepared_thread.user_stack_top = new_image.stack_pointer;
    prepared_thread.user_stack_bottom = new_image.stack_pointer -
                                        (uintptr_t)KERNEL_USER_STACK_SIZE;
    for (size_t reg = 0; reg < 15; ++reg) prepared_thread.registers[reg] = 0;
    if (kernel_thread_set_startup(&prepared_thread,
                                  argument_count, arguments,
                                  environment_count, environment) != 0) {
        kernel_user_image_destroy(&new_image);
        *result = -KERNEL_ERROR_TOO_BIG;
        return 1;
    }

    KernelThread *thread = scheduler_current;
    KernelProcess *process = thread->process;
    scheduler_request_process_stop(process, thread);
    kernel_smp_reschedule_others();
    kernel_smp_switch_release();
    while (kernel_smp_process_other_thread_active(process, thread)) {
        cvm_fence();
        cvm_nop();
    }
    kernel_smp_switch_reacquire();
    KernelUserImage old_image;
    scheduler_copy_user_image(&old_image, &process->image);
    scheduler_exec_discard_other_threads(process, thread);
    process->stop_requested = 0;
    process->stop_owner = NULL;
    kernel_display_release_process(process);
    scheduler_copy_user_image(&process->image, &new_image);
    process->next_stack_slot = 1;
    process->exit_status = -1;
    process->fault_cause = 0;
    process->fault_address = 0;
    process->fault_info = 0;
    process->faulted = 0;
    process->state = KERNEL_PROCESS_ACTIVE;

    for (size_t reg = 0; reg < 15; ++reg) {
        thread->registers[reg] = prepared_thread.registers[reg];
        TRAP_REGISTER(frame, reg) = prepared_thread.registers[reg];
    }
    thread->pc = new_image.entry;
    thread->flags = KERNEL_CPU_FLAG_INTERRUPT_ENABLE;
    thread->stack_pointer = prepared_thread.stack_pointer;
    thread->user_stack_bottom = prepared_thread.user_stack_bottom;
    thread->user_stack_top = prepared_thread.user_stack_top;
    thread->exit_status = -1;
    thread->wait_pid = 0;
    thread->wait_status_address = 0;
    thread->wait_tid = 0;
    thread->join_status_address = 0;
    thread->join_waiter = NULL;
    thread->state = KERNEL_THREAD_RUNNING;
    TRAP_PC(frame) = thread->pc;
    TRAP_FLAGS(frame) = thread->flags | KERNEL_SAVED_USER_MODE |
                        (TRAP_FLAGS(frame) & KERNEL_SAVED_EXCEPTION_FRAME);
    TRAP_USER_SP(frame) = thread->stack_pointer;
    cvm_set_ptbr((uint64_t)kernel_address_space_root(
        process->image.address_space));
    cvm_set_ksp((uint64_t)thread->kernel_stack_top);
    kernel_user_image_destroy(&old_image);
    ++scheduler_exec_count;
    kernel_smp_reschedule_others();
    return 0;
}

void kernel_scheduler_exit(uint64_t *frame, int64_t status)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    scheduler_reap_deferred();
    KernelThread *previous = scheduler_current;
    scheduler_save(previous, frame);
    previous->exit_status = status;
    previous->wait_pid = 0;
    previous->wait_status_address = 0;
    previous->wait_tid = 0;
    previous->join_status_address = 0;
    previous->state = KERNEL_THREAD_ZOMBIE;
    KernelProcess *process = previous->process;
    if (status != 0 && process->exit_status <= 0) {
        process->exit_status = status;
    }
    if (process->live_thread_count != 0) --process->live_thread_count;
    scheduler_notify_joiner(previous);
    if (process->live_thread_count == 0) {
        kernel_display_release_process(process);
        if (process->exit_status == -1) process->exit_status = 0;
        process->state = KERNEL_PROCESS_ZOMBIE;
        scheduler_record_process_completion(process);
    }

    if (scheduler_retire_secondary_frame(frame)) return;

    KernelThread *next = scheduler_dequeue_or_idle();
    if (next == NULL) scheduler_complete_or_wait();
    scheduler_set_current(next);
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
        if (thread->wait_tid != 0) {
            KernelThread *target = scheduler_find_thread(process,
                                                         thread->wait_tid);
            if (target != NULL && target->join_waiter == thread) {
                target->join_waiter = NULL;
            }
        }
        if (thread->join_waiter != NULL) {
            thread->join_waiter->wait_tid = 0;
            thread->join_waiter->join_status_address = 0;
            thread->join_waiter = NULL;
        }
        if (thread->queued) {
            kernel_list_remove(&scheduler_run_queue, &thread->run_node);
            thread->queued = 0;
        }
        scheduler_detach_wait(thread);
        if (thread->state != KERNEL_THREAD_DEAD) {
            thread->exit_status = status;
            thread->wait_pid = 0;
            thread->wait_status_address = 0;
            thread->state = KERNEL_THREAD_ZOMBIE;
        }
    }
    process->live_thread_count = 0;
    process->exit_status = status;
    process->fault_cause = cause;
    process->fault_address = address;
    process->fault_info = info;
    process->faulted = 1;
    process->stop_owner = NULL;
    process->stop_requested = 1;
    kernel_smp_reschedule_others();
    kernel_smp_switch_release();
    while (kernel_smp_process_other_thread_active(process, previous)) {
        cvm_fence();
        cvm_nop();
    }
    kernel_smp_switch_reacquire();
    process->state = KERNEL_PROCESS_ZOMBIE;
    kernel_display_release_process(process);
    scheduler_record_process_completion(process);

    if (scheduler_retire_secondary_frame(frame)) return;

    KernelThread *next = scheduler_dequeue_or_idle();
    if (next == NULL) scheduler_complete_or_wait();
    scheduler_set_current(next);
    scheduler_load(previous, next, frame);
}

void kernel_scheduler_timer(uint64_t *frame)
{
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_STATUS_OFFSET,
                     TIMER_STATUS_PENDING);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS + IRQ_CONTROLLER_EOI_OFFSET,
                     KERNEL_TIMER_INTERRUPT_LINE);
    if (frame == NULL) return;
    if (kernel_scheduler_stop_current(frame)) return;
    ++scheduler_timer_count;
    scheduler_expire_timeouts();
    kernel_scheduler_interrupt_return(frame);
    if (kernel_smp_idle_active()) {
        return;
    }
    if ((TRAP_FLAGS(frame) & KERNEL_SAVED_USER_MODE) == 0 ||
        kernel_scheduler_current_space() == NULL) {
        return;
    }
    scheduler_reap_deferred();
    scheduler_reschedule(frame, 1);
}

void kernel_scheduler_ipi(uint64_t *frame)
{
    if (frame == NULL) return;
    kernel_scheduler_interrupt_return(frame);
    if (kernel_scheduler_stop_current(frame)) return;
    if (kernel_smp_idle_active()) return;
    if ((TRAP_FLAGS(frame) & KERNEL_SAVED_USER_MODE) == 0 ||
        kernel_scheduler_current_space() == NULL) {
        return;
    }
    scheduler_reap_deferred();
    scheduler_reschedule(frame, 1);
}

void kernel_scheduler_secondary_start(void)
{
    kernel_smp_switch_reacquire();
    if (!scheduler_active) {
        kernel_smp_switch_release();
        return;
    }
    scheduler_reap_deferred();
    KernelThread *next = scheduler_dequeue();
    if (next == NULL) {
        if (kernel_smp_is_boot_cpu() && !scheduler_has_blocked_thread() &&
            !kernel_smp_has_running_thread()) {
            scheduler_finish();
        }
        kernel_smp_switch_release();
        return;
    }
    scheduler_set_current(next);
    ++scheduler_secondary_dispatch_count;
    if (next->kernel_resume_sp != 0) {
        uintptr_t resume_sp = next->kernel_resume_sp;
        next->kernel_resume_sp = 0;
        cvm_set_ptbr((uint64_t)kernel_address_space_root(
            next->process->image.address_space));
        cvm_set_ksp((uint64_t)next->kernel_stack_top);
        kernel_smp_switch_release();
        kernel_resume_continuation(resume_sp);
        return;
    }
    uint64_t root = (uint64_t)kernel_address_space_root(
        next->process->image.address_space);
    uint64_t pc = next->pc;
    uint64_t stack = next->stack_pointer;
    uint64_t kernel_stack = next->kernel_stack_top;
    const uint64_t *registers = next->registers;
    kernel_smp_switch_release();
    kernel_start_user(root, pc, stack, kernel_stack, registers);
}

static KernelProcess *scheduler_make_init_process(void)
{
    scheduler_init_from_disk = 0;
    KernelProcess *process = kernel_process_create_path(
        "/BIN/INIT.EXF", KERNEL_INIT_IMAGE_MAXIMUM);
    if (process == NULL) return NULL;
    for (size_t i = 0; i < 2; ++i) {
        if (kernel_thread_create(process) == NULL) {
            kernel_process_destroy(process);
            return NULL;
        }
    }
    KernelThread *initial = scheduler_process_thread_from_node(
        process->threads.sentinel.next);
    static const char *const arguments[] = {"/BIN/INIT.EXF"};
    static const char *const environment[] = {"PATH=/BIN"};
    if (kernel_thread_set_startup(initial, 1, arguments, 1, environment) != 0) {
        kernel_process_destroy(process);
        return NULL;
    }
    scheduler_init_from_disk = 1;
    return process;
}

static KernelProcess *scheduler_make_fault_process(KernelProcess *parent,
                                                   size_t thread_count)
{
    size_t image_size;
    uint8_t *data = kernel_user_fault_test_program_create(&image_size);
    if (data == NULL) return NULL;
    KernelProcess *process = parent != NULL
                                 ? kernel_process_create_child(
                                       parent, data, image_size)
                                 : kernel_process_create(data, image_size);
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

static int scheduler_registration_rollback_test(KernelProcess *process)
{
    if (process == NULL || process->thread_count < 2) return 0;
    KernelThread *first = scheduler_process_thread_from_node(
        process->threads.sentinel.next);
    KernelThread *invalid = scheduler_process_thread_from_node(
        process->threads.sentinel.next->next);
    invalid->state = KERNEL_THREAD_RUNNABLE;
    int rejected = !kernel_scheduler_register_process(process) &&
                   !process->registered && scheduler_processes.count == 0 &&
                   scheduler_run_queue.count == 0 &&
                   scheduler_process_count == 0 && scheduler_thread_count == 0 &&
                   first->state == KERNEL_THREAD_NEW && !first->queued;
    invalid->state = KERNEL_THREAD_NEW;
    return rejected;
}

static int scheduler_orphan_policy_test(KernelProcess *init)
{
    if (init == NULL || scheduler_init_process != init) return 0;
    KernelProcess parent;
    KernelProcess child;
    kernel_list_init(&parent.children);
    child.parent = &parent;
    child.state = KERNEL_PROCESS_ACTIVE;
    child.reap_queued = 0;
    kernel_list_push_back(&parent.children, &child.child_node);
    size_t original_count = init->children.count;
    scheduler_orphan_children(&parent);
    int okay = parent.children.count == 0 && child.parent == init &&
               init->children.count == original_count + 1;
    if (child.parent == init) {
        kernel_list_remove(&init->children, &child.child_node);
        child.parent = NULL;
    }
    return okay && init->children.count == original_count;
}

static int scheduler_wait_queue_test(void)
{
    KernelWaitQueue queue;
    KernelThread first;
    KernelThread second;
    kernel_wait_queue_init(&queue);
    first.state = KERNEL_THREAD_BLOCKED;
    second.state = KERNEL_THREAD_BLOCKED;
    first.queued = 0;
    second.queued = 0;
    first.cpu_affinity = 0;
    second.cpu_affinity = 0;
    first.timeout_queued = 0;
    second.timeout_queued = 0;
    first.wait_pid = 0;
    second.wait_pid = 0;
    first.wait_status_address = 0;
    second.wait_status_address = 0;
    first.wait_tid = 0;
    second.wait_tid = 0;
    first.join_status_address = 0;
    second.join_status_address = 0;
    first.wait_queue = &queue;
    second.wait_queue = &queue;
    kernel_list_push_back(&queue.waiters, &first.wait_node);
    kernel_list_push_back(&queue.waiters, &second.wait_node);
    if (kernel_wait_queue_wake_one(&queue, 11) != 1 ||
        first.state != KERNEL_THREAD_RUNNABLE || first.registers[0] != 11 ||
        queue.waiters.count != 1 || scheduler_run_queue.count != 1 ||
        kernel_wait_queue_wake_all(&queue, 22) != 1 ||
        second.state != KERNEL_THREAD_RUNNABLE || second.registers[0] != 22 ||
        !kernel_list_empty(&queue.waiters) || scheduler_run_queue.count != 2 ||
        kernel_wait_queue_wake_one(&queue, 33) != 0) {
        return 0;
    }
    if (scheduler_dequeue() != &first || scheduler_dequeue() != &second ||
        scheduler_dequeue() != NULL ||
        !kernel_list_empty(&scheduler_run_queue)) {
        return 0;
    }

    /* Completion before timeout must remove both queue links. */
    scheduler_timer_count = 100;
    first.state = KERNEL_THREAD_BLOCKED;
    first.wait_queue = &queue;
    first.wait_deadline = 102;
    first.wait_timeout_result = -KERNEL_ERROR_AGAIN;
    first.timeout_queued = 1;
    kernel_list_push_back(&queue.waiters, &first.wait_node);
    kernel_list_push_back(&scheduler_timeout_queue, &first.timeout_node);
    if (kernel_wait_queue_wake_one(&queue, 44) != 1 ||
        first.timeout_queued || first.wait_queue != NULL ||
        !kernel_list_empty(&queue.waiters) ||
        !kernel_list_empty(&scheduler_timeout_queue) ||
        scheduler_dequeue() != &first) {
        return 0;
    }

    /* Timeout before completion must make a late IRQ wake a no-op. */
    second.state = KERNEL_THREAD_BLOCKED;
    second.wait_queue = &queue;
    second.wait_deadline = scheduler_timer_count;
    second.wait_timeout_result = -KERNEL_ERROR_AGAIN;
    second.timeout_queued = 1;
    kernel_list_push_back(&queue.waiters, &second.wait_node);
    kernel_list_push_back(&scheduler_timeout_queue, &second.timeout_node);
    scheduler_expire_timeouts();
    int okay = second.registers[0] ==
                   (uint64_t)-(int64_t)KERNEL_ERROR_AGAIN &&
               !second.timeout_queued && second.wait_queue == NULL &&
               kernel_wait_queue_wake_one(&queue, 55) == 0 &&
               scheduler_dequeue() == &second &&
               scheduler_dequeue() == NULL &&
               kernel_list_empty(&scheduler_timeout_queue);
    scheduler_timer_count = 0;
    return okay;
}

static int scheduler_validate_test_layout(KernelProcess *first,
                                           KernelProcess *child)
{
    if (first == NULL || child == NULL || first->pid != 1 || child->pid == 0 ||
        first->pid == child->pid || first->thread_count != 2 ||
        child->thread_count != 2 || child->parent != first ||
        first->children.count != 1 || first->fd_table == NULL ||
        child->fd_table == NULL || first->fd_table == child->fd_table ||
        kernel_fd_open_count(first->fd_table) != 3 ||
        kernel_fd_open_count(child->fd_table) != 3) {
        return 0;
    }
    uintptr_t first_root = kernel_address_space_root(
        first->image.address_space);
    uintptr_t child_root = kernel_address_space_root(
        child->image.address_space);
    if (first_root == child_root) {
        return 0;
    }
    KernelThread *thread_a = scheduler_process_thread_from_node(
        first->threads.sentinel.next);
    KernelThread *thread_b = scheduler_process_thread_from_node(
        first->threads.sentinel.next->next);
    int registers_valid = scheduler_init_from_disk
                              ? thread_a->registers[0] == 1 &&
                                    thread_a->registers[4] == child->pid &&
                                    thread_a->registers[5] == thread_b->tid &&
                                    thread_a->registers[6] ==
                                        (uint64_t)-(int64_t)
                                            KERNEL_EXCEPTION_LOAD_PAGE_FAULT &&
                                    thread_b->registers[4] == 0
                              : thread_a->registers[1] == child->pid &&
                                    thread_a->registers[3] ==
                                        (uint64_t)-(int64_t)
                                            KERNEL_EXCEPTION_LOAD_PAGE_FAULT &&
                                    thread_a->registers[0] == thread_b->tid &&
                                    thread_b->registers[1] == 0;
    return thread_a->process == thread_b->process && registers_valid &&
           thread_a->stack_pointer != thread_b->stack_pointer &&
           thread_a->kernel_stack_top != thread_b->kernel_stack_top;
}

static int scheduler_wait_read_test(KernelProcess *process)
{
    if (process == NULL || kernel_list_empty(&process->threads)) return 0;
    KernelThread *thread = scheduler_process_thread_from_node(
        process->threads.sentinel.next);
    KernelWaitQueue queue;
    kernel_wait_queue_init(&queue);
    uintptr_t destination = thread->user_stack_bottom;
    uint64_t saved_r0 = thread->registers[0];
    thread->state = KERNEL_THREAD_BLOCKED;
    thread->wait_queue = &queue;
    thread->wait_buffer_address = destination;
    thread->wait_buffer_size = 1;
    kernel_list_push_back(&queue.waiters, &thread->wait_node);
    uint8_t input = 0x29;
    uint8_t output = 0;
    int okay = kernel_wait_queue_wake_read_one(&queue, &input, 1) == 1 &&
               kernel_copy_from_user(process->image.address_space,
                                     &output, destination, 1) &&
               output == input && thread->registers[0] == 1 &&
               thread->wait_queue == NULL &&
               thread->wait_buffer_size == 0 &&
               scheduler_dequeue() == thread &&
               kernel_wait_queue_wake_read_one(&queue, &input, 1) == 0;
    thread->state = KERNEL_THREAD_NEW;
    thread->registers[0] = saved_r0;
    return okay && kernel_list_empty(&queue.waiters) &&
           kernel_list_empty(&scheduler_run_queue);
}

int kernel_scheduler_self_test(void)
{
    scheduler_active = 0;
    scheduler_set_current(NULL);
    scheduler_init_process = NULL;
    scheduler_process_count = 0;
    scheduler_thread_count = 0;
    scheduler_timer_count = 0;
    scheduler_preemption_count = 0;
    scheduler_completed_process_count = 0;
    scheduler_faulted_process_count = 0;
    scheduler_reaped_process_count = 0;
    scheduler_wait_block_count = 0;
    scheduler_wait_wake_count = 0;
    scheduler_wait_reap_count = 0;
    scheduler_join_block_count = 0;
    scheduler_join_wake_count = 0;
    scheduler_join_reap_count = 0;
    scheduler_exec_count = 0;
    scheduler_sleep_block_count = 0;
    scheduler_sleep_wake_count = 0;
    scheduler_idle_count = 0;
    scheduler_kernel_block_switch_count = 0;
    scheduler_secondary_dispatch_count = 0;
    scheduler_same_process_parallel_count = 0;
    scheduler_completion_error = 0;
    scheduler_init_from_disk = 0;
    kernel_list_init(&scheduler_processes);
    kernel_list_init(&scheduler_run_queue);
    kernel_list_init(&scheduler_reap_queue);
    kernel_list_init(&scheduler_thread_reap_queue);
    kernel_list_init(&scheduler_timeout_queue);
    kernel_wait_queue_init(&scheduler_sleep_queue);
    kernel_process_system_init();

    if (!scheduler_wait_queue_test()) return 1;
    KernelProcess *first = scheduler_make_init_process();
    if (scheduler_init_from_disk) scheduler_init_process = first;
    KernelProcess *child = scheduler_make_fault_process(first, 2);
    if (first != NULL && child != NULL) {
        KernelThread *coordinator = scheduler_process_thread_from_node(
            first->threads.sentinel.next);
        KernelThread *worker = scheduler_process_thread_from_node(
            first->threads.sentinel.next->next);
        if (scheduler_init_from_disk) {
            coordinator->registers[4] = child->pid;
            coordinator->registers[5] = worker->tid;
            coordinator->registers[6] =
                (uint64_t)-(int64_t)KERNEL_EXCEPTION_LOAD_PAGE_FAULT;
        } else {
            coordinator->registers[1] = child->pid;
            coordinator->registers[3] =
                (uint64_t)-(int64_t)KERNEL_EXCEPTION_LOAD_PAGE_FAULT;
        }
    }
    if (!scheduler_validate_test_layout(first, child) ||
        !scheduler_wait_read_test(first) ||
        !scheduler_registration_rollback_test(first) ||
        !scheduler_orphan_policy_test(first)) {
        scheduler_init_process = NULL;
        kernel_process_destroy(first);
        kernel_process_destroy(child);
        return 1;
    }
    if (!kernel_scheduler_register_process(first) ||
        !kernel_scheduler_register_process(child)) {
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
                     KERNEL_TIMER_QUANTUM_TICKS);
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_STATUS_OFFSET,
                     TIMER_STATUS_PENDING);
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_CONTROL_OFFSET,
                     TIMER_CONTROL_ENABLE | TIMER_CONTROL_REPEAT |
                         TIMER_CONTROL_IRQ_ENABLE);
    if (!kernel_devices_enable_interrupts()) {
        scheduler_destroy_processes();
        return 1;
    }
    scheduler_set_current(scheduler_dequeue());
    if (scheduler_current == NULL) {
        scheduler_destroy_processes();
        return 1;
    }
    scheduler_active = 1;
    if (!kernel_smp_kick_secondaries()) {
        scheduler_active = 0;
        scheduler_destroy_processes();
        return 1;
    }
    if (kernel_smp_logical_count() > 1) {
        for (uint64_t wait = 0;
             wait < KERNEL_SECONDARY_DISPATCH_WAIT &&
             scheduler_secondary_dispatch_count == 0;
             ++wait) {
            cvm_fence();
            cvm_nop();
        }
    }
    kernel_uart_puts("KERNEL: USER TASKS START\n");
    kernel_start_user(
        kernel_address_space_root(
            scheduler_current->process->image.address_space),
        scheduler_current->pc,
        scheduler_current->stack_pointer,
        scheduler_current->kernel_stack_top,
        scheduler_current->registers);
    return 1;
}
