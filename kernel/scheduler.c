#include "kernel_internal.h"

#include "builtin_device_protocol.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

#define KERNEL_TIMER_QUANTUM 2U
#define KERNEL_SCHEDULER_TEST_PROCESSES 2U
#define KERNEL_SCHEDULER_TEST_THREADS 3U
#define KERNEL_INIT_IMAGE_MAXIMUM ((size_t)4 * 1024 * 1024)

#define TRAP_REGISTER(frame, reg) ((frame)[15U - (reg)])
#define TRAP_PC(frame) ((frame)[16])
#define TRAP_FLAGS(frame) ((frame)[17])
#define TRAP_USER_SP(frame) ((frame)[18])

static KernelList scheduler_processes;
static KernelList scheduler_run_queue;
static KernelList scheduler_reap_queue;
static KernelList scheduler_thread_reap_queue;
static KernelThread *scheduler_current;
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
static int scheduler_completion_error;
static int scheduler_active;
static int scheduler_init_from_disk;

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

static void scheduler_queue_reap(KernelProcess *process)
{
    if (process == NULL || process->parent != NULL || process->reap_queued) {
        return;
    }
    kernel_list_push_back(&scheduler_reap_queue, &process->reap_node);
    process->reap_queued = 1;
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
    thread->registers[0] = (uint64_t)result;
    thread->wait_pid = 0;
    thread->wait_status_address = 0;
    thread->wait_tid = 0;
    thread->join_status_address = 0;
    thread->state = KERNEL_THREAD_RUNNABLE;
    if (!scheduler_enqueue(thread)) scheduler_completion_error = 1;
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
        if (process == scheduler_init_process) scheduler_init_process = NULL;
        process->registered = 0;
        kernel_process_destroy(process);
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
        if (thread == scheduler_current || kernel_thread_destroy(thread) != 0) {
            scheduler_completion_error = 1;
        }
    }
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
        if (process == scheduler_init_process) scheduler_init_process = NULL;
        process->registered = 0;
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
                  scheduler_init_from_disk &&
                  scheduler_process_count == KERNEL_SCHEDULER_TEST_PROCESSES &&
                  scheduler_thread_count == KERNEL_SCHEDULER_TEST_THREADS &&
                  scheduler_completed_process_count ==
                      KERNEL_SCHEDULER_TEST_PROCESSES &&
                  scheduler_faulted_process_count == 1 &&
                  scheduler_wait_block_count == 1 &&
                  scheduler_wait_wake_count == 1 &&
                  scheduler_wait_reap_count == 1 &&
                  scheduler_join_block_count == 1 &&
                  scheduler_join_wake_count == 1 &&
                  scheduler_join_reap_count == 1 &&
                  scheduler_exec_count == 1 &&
                  scheduler_reaped_process_count + 1 ==
                      scheduler_completed_process_count &&
                  scheduler_processes.count == 1 &&
                  scheduler_reap_queue.count == 1 &&
                  scheduler_completion_error == 0 && heap_valid;
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

    KernelThread *next = scheduler_dequeue();
    if (next == NULL) {
        *result = -KERNEL_ERROR_AGAIN;
        return 1;
    }
    scheduler_save(waiter, frame);
    waiter->wait_pid = pid;
    waiter->wait_status_address = status_address;
    waiter->state = KERNEL_THREAD_BLOCKED;
    ++scheduler_wait_block_count;
    scheduler_current = next;
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

    KernelThread *next = scheduler_dequeue();
    if (next == NULL) {
        *result = -KERNEL_ERROR_AGAIN;
        return 1;
    }
    scheduler_save(waiter, frame);
    waiter->wait_tid = tid;
    waiter->join_status_address = status_address;
    waiter->state = KERNEL_THREAD_BLOCKED;
    target->join_waiter = waiter;
    ++scheduler_join_block_count;
    scheduler_current = next;
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
    KernelUserImage old_image;
    scheduler_copy_user_image(&old_image, &process->image);
    scheduler_exec_discard_other_threads(process, thread);
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

static int scheduler_validate_test_layout(KernelProcess *first,
                                           KernelProcess *child)
{
    if (first == NULL || child == NULL || first->pid != 1 || child->pid == 0 ||
        first->pid == child->pid || first->thread_count != 2 ||
        child->thread_count != 1 || child->parent != first ||
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

int kernel_scheduler_self_test(void)
{
    scheduler_active = 0;
    scheduler_current = NULL;
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
    scheduler_completion_error = 0;
    scheduler_init_from_disk = 0;
    kernel_list_init(&scheduler_processes);
    kernel_list_init(&scheduler_run_queue);
    kernel_list_init(&scheduler_reap_queue);
    kernel_list_init(&scheduler_thread_reap_queue);
    kernel_process_system_init();

    KernelProcess *first = scheduler_make_init_process();
    if (scheduler_init_from_disk) scheduler_init_process = first;
    KernelProcess *child = scheduler_make_fault_process(first, 1);
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
        scheduler_current->kernel_stack_top,
        scheduler_current->registers);
    return 1;
}
