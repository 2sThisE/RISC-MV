#include "kernel_internal.h"
#include "kernel_runtime.h"

#include <cvm/intrin.h>

static uint64_t process_next_pid;
static uint64_t process_next_tid;

static KernelThread *process_thread_from_node(KernelListNode *node)
{
    return (KernelThread *)((uint8_t *)node -
                            offsetof(KernelThread, process_node));
}

static void process_zero_thread_context(KernelThread *thread)
{
    for (size_t reg = 0; reg < 15; ++reg) thread->registers[reg] = 0;
}

static int process_map_thread_stack(KernelProcess *process,
                                    uint64_t slot,
                                    uintptr_t *stack_pointer)
{
    uint64_t stride = KERNEL_USER_STACK_SIZE + KERNEL_PAGE_SIZE;
    uint64_t available = KERNEL_USER_STACK_TOP - KERNEL_USER_IMAGE_LIMIT;
    if (slot > available / stride) return 0;
    uint64_t offset = slot * stride;
    if (offset > KERNEL_USER_STACK_TOP) return 0;
    uintptr_t top = (uintptr_t)(KERNEL_USER_STACK_TOP - offset);
    if (top < KERNEL_USER_STACK_SIZE) return 0;
    uintptr_t bottom = top - (uintptr_t)KERNEL_USER_STACK_SIZE;
    if (bottom < (uintptr_t)KERNEL_USER_IMAGE_LIMIT) return 0;
    for (uintptr_t page = bottom; page < top;
         page += (uintptr_t)KERNEL_PAGE_SIZE) {
        uintptr_t physical;
        if (kernel_address_space_map_anonymous(
                process->image.address_space, page,
                KERNEL_PTE_READ | KERNEL_PTE_WRITE, &physical) != 0) {
            return 0;
        }
    }
    *stack_pointer = top;
    return 1;
}

void kernel_process_system_init(void)
{
    process_next_pid = 1;
    process_next_tid = 1;
}

KernelProcess *kernel_process_create(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0 || process_next_pid == 0) return NULL;
    KernelProcess *process = kernel_calloc(1, sizeof(*process));
    if (process == NULL) return NULL;
    kernel_list_init(&process->threads);
    if (kernel_user_image_load(data, size, &process->image) != 0) {
        kernel_free(process);
        return NULL;
    }
    process->pid = process_next_pid++;
    process->exit_status = -1;
    process->state = KERNEL_PROCESS_NEW;
    return process;
}

KernelThread *kernel_thread_create(KernelProcess *process)
{
    if (process == NULL || process->image.address_space == NULL ||
        process->state == KERNEL_PROCESS_ZOMBIE || process_next_tid == 0) {
        return NULL;
    }
    KernelThread *thread = kernel_calloc(1, sizeof(*thread));
    if (thread == NULL) return NULL;
    thread->kernel_stack = kernel_malloc(KERNEL_THREAD_KERNEL_STACK_SIZE);
    if (thread->kernel_stack == NULL) {
        kernel_free(thread);
        return NULL;
    }

    uint64_t slot = process->next_stack_slot;
    if (slot == 0) {
        thread->stack_pointer = process->image.stack_pointer;
    } else if (!process_map_thread_stack(process, slot,
                                         &thread->stack_pointer)) {
        kernel_free(thread->kernel_stack);
        kernel_free(thread);
        return NULL;
    }
    ++process->next_stack_slot;

    thread->process = process;
    thread->pc = process->image.entry;
    thread->flags = KERNEL_CPU_FLAG_INTERRUPT_ENABLE;
    thread->kernel_stack_top =
        (uintptr_t)(thread->kernel_stack + KERNEL_THREAD_KERNEL_STACK_SIZE);
    thread->kernel_stack_top &= ~(uintptr_t)UINT64_C(0xF);
    thread->tid = process_next_tid++;
    thread->exit_status = -1;
    thread->state = KERNEL_THREAD_NEW;
    process_zero_thread_context(thread);
    kernel_list_push_back(&process->threads, &thread->process_node);
    ++process->thread_count;
    ++process->live_thread_count;
    process->state = KERNEL_PROCESS_ACTIVE;
    return thread;
}

void kernel_process_destroy(KernelProcess *process)
{
    if (process == NULL) return;
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&process->threads)) != NULL) {
        KernelThread *thread = process_thread_from_node(node);
        thread->state = KERNEL_THREAD_DEAD;
        kernel_free(thread->kernel_stack);
        kernel_free(thread);
    }
    kernel_user_image_destroy(&process->image);
    kernel_free(process);
}
