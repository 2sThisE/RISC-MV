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
                                    uintptr_t *stack_bottom,
                                    uintptr_t *stack_top)
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
    uintptr_t mapped_end = bottom;
    for (uintptr_t page = bottom; page < top;
         page += (uintptr_t)KERNEL_PAGE_SIZE) {
        uintptr_t physical;
        if (kernel_address_space_map_anonymous(
                process->image.address_space, page,
                KERNEL_PTE_READ | KERNEL_PTE_WRITE, &physical) != 0) {
            if (mapped_end != bottom) {
                (void)kernel_address_space_unmap_range(
                    process->image.address_space, bottom,
                    (size_t)(mapped_end - bottom));
            }
            return 0;
        }
        mapped_end = page + (uintptr_t)KERNEL_PAGE_SIZE;
    }
    *stack_bottom = bottom;
    *stack_top = top;
    return 1;
}

void kernel_process_system_init(void)
{
    process_next_pid = 1;
    process_next_tid = 1;
}

static KernelProcess *process_adopt_image(KernelProcess *parent,
                                          KernelUserImage *image)
{
    if (image == NULL || image->address_space == NULL || process_next_pid == 0 ||
        (parent != NULL && parent->state == KERNEL_PROCESS_ZOMBIE)) {
        return NULL;
    }
    KernelProcess *process = kernel_calloc(1, sizeof(*process));
    if (process == NULL) return NULL;
    process->fd_table = kernel_fd_table_create();
    if (process->fd_table == NULL ||
        !kernel_fd_populate_standard(process->fd_table)) {
        kernel_fd_table_destroy(process->fd_table);
        kernel_free(process);
        return NULL;
    }
    kernel_list_init(&process->threads);
    kernel_list_init(&process->children);
    process->image.address_space = image->address_space;
    process->image.entry = image->entry;
    process->image.stack_pointer = image->stack_pointer;
    process->image.image_base = image->image_base;
    process->image.image_end = image->image_end;
    image->address_space = NULL;
    image->entry = 0;
    image->stack_pointer = 0;
    image->image_base = 0;
    image->image_end = 0;
    process->pid = process_next_pid++;
    process->exit_status = -1;
    process->state = KERNEL_PROCESS_NEW;
    if (parent != NULL) {
        process->parent = parent;
        kernel_list_push_back(&parent->children, &process->child_node);
    }
    return process;
}

static KernelProcess *process_create(KernelProcess *parent,
                                     const uint8_t *data,
                                     size_t size)
{
    if (data == NULL || size == 0) return NULL;
    KernelUserImage image;
    if (kernel_user_image_load(data, size, &image) != 0) return NULL;
    KernelProcess *process = process_adopt_image(parent, &image);
    if (process == NULL) kernel_user_image_destroy(&image);
    return process;
}

static KernelProcess *process_create_path(KernelProcess *parent,
                                          const char *path,
                                          size_t maximum_size)
{
    if (path == NULL || maximum_size == 0) return NULL;
    KernelUserImage image;
    if (kernel_user_image_load_path(path, maximum_size, &image) != 0) return NULL;
    KernelProcess *process = process_adopt_image(parent, &image);
    if (process == NULL) kernel_user_image_destroy(&image);
    return process;
}

KernelProcess *kernel_process_create(const uint8_t *data, size_t size)
{
    return process_create(NULL, data, size);
}

KernelProcess *kernel_process_create_child(KernelProcess *parent,
                                           const uint8_t *data,
                                           size_t size)
{
    if (parent == NULL) return NULL;
    return process_create(parent, data, size);
}

KernelProcess *kernel_process_create_path(const char *path,
                                          size_t maximum_size)
{
    return process_create_path(NULL, path, maximum_size);
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
        thread->user_stack_top = process->image.stack_pointer;
        thread->user_stack_bottom = thread->user_stack_top -
                                    (uintptr_t)KERNEL_USER_STACK_SIZE;
    } else if (!process_map_thread_stack(process, slot,
                                         &thread->user_stack_bottom,
                                         &thread->user_stack_top)) {
        kernel_free(thread->kernel_stack);
        kernel_free(thread);
        return NULL;
    }
    thread->stack_pointer = thread->user_stack_top;
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

static int process_copy_user_u64(KernelThread *thread,
                                 uintptr_t address,
                                 uint64_t value)
{
    return kernel_copy_to_user(thread->process->image.address_space,
                               address, &value, sizeof(value));
}

static int process_string_size(const char *text, size_t *size)
{
    if (text == NULL || size == NULL) return 0;
    size_t length = 0;
    while (text[length] != '\0') {
        if (length >= (size_t)KERNEL_USER_STACK_SIZE - 1) return 0;
        ++length;
    }
    *size = length + 1;
    return 1;
}

int kernel_thread_set_startup(KernelThread *thread,
                              size_t argument_count,
                              const char *const *arguments,
                              size_t environment_count,
                              const char *const *environment)
{
    enum { STARTUP_VECTOR_LIMIT = 32 };
    if (thread == NULL || thread->process == NULL ||
        thread->state != KERNEL_THREAD_NEW ||
        argument_count > STARTUP_VECTOR_LIMIT ||
        environment_count > STARTUP_VECTOR_LIMIT ||
        (argument_count != 0 && arguments == NULL) ||
        (environment_count != 0 && environment == NULL)) {
        return 1;
    }
    uintptr_t argument_addresses[STARTUP_VECTOR_LIMIT];
    uintptr_t environment_addresses[STARTUP_VECTOR_LIMIT];
    uintptr_t cursor = thread->user_stack_top;
    for (size_t i = 0; i < argument_count; ++i) {
        size_t length;
        if (!process_string_size(arguments[i], &length) ||
            cursor < thread->user_stack_bottom + length) {
            return 1;
        }
        cursor -= length;
        if (!kernel_copy_to_user(thread->process->image.address_space,
                                 cursor, arguments[i], length)) {
            return 1;
        }
        argument_addresses[i] = cursor;
    }
    for (size_t i = 0; i < environment_count; ++i) {
        size_t length;
        if (!process_string_size(environment[i], &length) ||
            cursor < thread->user_stack_bottom + length) {
            return 1;
        }
        cursor -= length;
        if (!kernel_copy_to_user(thread->process->image.address_space,
                                 cursor, environment[i], length)) {
            return 1;
        }
        environment_addresses[i] = cursor;
    }

    size_t word_count = 1 + argument_count + 1 + environment_count + 1;
    if (word_count > KERNEL_SIZE_MAX / sizeof(uint64_t)) return 1;
    size_t table_size = word_count * sizeof(uint64_t);
    if (cursor < thread->user_stack_bottom + table_size) return 1;
    uintptr_t stack_pointer = (cursor - table_size) & ~(uintptr_t)UINT64_C(0xF);
    if (stack_pointer < thread->user_stack_bottom) return 1;
    uintptr_t slot = stack_pointer;
    if (!process_copy_user_u64(thread, slot, argument_count)) return 1;
    slot += sizeof(uint64_t);
    for (size_t i = 0; i < argument_count; ++i) {
        if (!process_copy_user_u64(thread, slot, argument_addresses[i])) return 1;
        slot += sizeof(uint64_t);
    }
    if (!process_copy_user_u64(thread, slot, 0)) return 1;
    slot += sizeof(uint64_t);
    uintptr_t environment_pointer = slot;
    for (size_t i = 0; i < environment_count; ++i) {
        if (!process_copy_user_u64(thread, slot, environment_addresses[i])) return 1;
        slot += sizeof(uint64_t);
    }
    if (!process_copy_user_u64(thread, slot, 0)) return 1;

    thread->stack_pointer = stack_pointer;
    thread->registers[0] = argument_count;
    thread->registers[1] = stack_pointer + sizeof(uint64_t);
    thread->registers[2] = environment_pointer;
    return 0;
}

int kernel_thread_destroy(KernelThread *thread)
{
    if (thread == NULL || thread->process == NULL || thread->queued ||
        (thread->state != KERNEL_THREAD_ZOMBIE &&
         thread->state != KERNEL_THREAD_DEAD)) {
        return 1;
    }
    KernelProcess *process = thread->process;
    int unmap_error = kernel_address_space_unmap_range(
        process->image.address_space, thread->user_stack_bottom,
        (size_t)(thread->user_stack_top - thread->user_stack_bottom));
    kernel_list_remove(&process->threads, &thread->process_node);
    if (process->thread_count != 0) --process->thread_count;
    thread->state = KERNEL_THREAD_DEAD;
    thread->process = NULL;
    kernel_free(thread->kernel_stack);
    kernel_free(thread);
    return unmap_error;
}

void kernel_process_destroy(KernelProcess *process)
{
    if (process == NULL) return;
    if (process->parent != NULL) {
        kernel_list_remove(&process->parent->children, &process->child_node);
        process->parent = NULL;
    }
    KernelListNode *child_node;
    while ((child_node = kernel_list_pop_front(&process->children)) != NULL) {
        KernelProcess *child = (KernelProcess *)((uint8_t *)child_node -
            offsetof(KernelProcess, child_node));
        child->parent = NULL;
    }
    KernelListNode *node;
    while ((node = kernel_list_pop_front(&process->threads)) != NULL) {
        KernelThread *thread = process_thread_from_node(node);
        thread->state = KERNEL_THREAD_DEAD;
        kernel_free(thread->kernel_stack);
        kernel_free(thread);
    }
    kernel_fd_table_destroy(process->fd_table);
    process->fd_table = NULL;
    kernel_user_image_destroy(&process->image);
    kernel_free(process);
}

static int process_lifetime_cycle(const uint8_t *data,
                                  size_t size,
                                  int destroy_parent_first)
{
    KernelProcess *parent = kernel_process_create(data, size);
    KernelProcess *child = NULL;
    if (parent == NULL || kernel_thread_create(parent) == NULL ||
        kernel_thread_create(parent) == NULL || parent->fd_table == NULL) {
        kernel_process_destroy(parent);
        return 1;
    }
    KernelThread *initial = process_thread_from_node(
        parent->threads.sentinel.next);
    static const char *const arguments[] = {"/BIN/INIT.EXF"};
    static const char *const environment[] = {"PATH=/BIN"};
    uint64_t argc_value;
    uint64_t argument_pointer;
    char argument_text[sizeof("/BIN/INIT.EXF")];
    if (kernel_thread_set_startup(initial, 1, arguments, 1, environment) != 0 ||
        (initial->stack_pointer & UINT64_C(0xF)) != 0 ||
        !kernel_copy_from_user(parent->image.address_space,
                               &argc_value, initial->stack_pointer,
                               sizeof(argc_value)) || argc_value != 1 ||
        !kernel_copy_from_user(parent->image.address_space,
                               &argument_pointer, initial->registers[1],
                               sizeof(argument_pointer)) ||
        !kernel_copy_from_user(parent->image.address_space,
                               argument_text, (uintptr_t)argument_pointer,
                               sizeof(argument_text))) {
        kernel_process_destroy(parent);
        return 1;
    }
    for (size_t i = 0; i < sizeof(argument_text); ++i) {
        if (argument_text[i] != arguments[0][i]) {
            kernel_process_destroy(parent);
            return 1;
        }
    }
    child = kernel_process_create_child(parent, data, size);
    if (child == NULL || kernel_thread_create(child) == NULL ||
        child->parent != parent || parent->children.count != 1) {
        kernel_process_destroy(child);
        kernel_process_destroy(parent);
        return 1;
    }
    if (destroy_parent_first) {
        kernel_process_destroy(parent);
        if (child->parent != NULL) {
            kernel_process_destroy(child);
            return 1;
        }
        kernel_process_destroy(child);
    } else {
        kernel_process_destroy(child);
        if (parent->children.count != 0) {
            kernel_process_destroy(parent);
            return 1;
        }
        kernel_process_destroy(parent);
    }
    return kernel_heap_validate();
}

int kernel_process_lifetime_self_test(void)
{
    size_t image_size;
    uint8_t *data;
    if (!kernel_vfs_read_all("/BIN/INIT.EXF",
                             (size_t)4 * 1024 * 1024,
                             &data,
                             &image_size)) {
        return 1;
    }
    kernel_process_system_init();
    if (process_lifetime_cycle(data, image_size, 0) != 0) {
        kernel_free(data);
        return 1;
    }
    uint64_t free_pages = kernel_pmm_free_page_count();
    for (size_t iteration = 0; iteration < 8; ++iteration) {
        if (process_lifetime_cycle(data, image_size,
                                   (int)(iteration & 1U)) != 0 ||
            kernel_pmm_free_page_count() != free_pages) {
            kernel_free(data);
            return 1;
        }
    }
    kernel_free(data);
    return kernel_heap_validate();
}
