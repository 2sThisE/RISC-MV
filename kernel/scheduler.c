#include "kernel_internal.h"

#include "builtin_device_protocol.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

#define KERNEL_TASK_COUNT 2U
#define KERNEL_TASK_RUNNABLE 1U
#define KERNEL_TASK_EXITED 2U
#define KERNEL_TIMER_QUANTUM 2U

#define TRAP_REGISTER(frame, reg) ((frame)[15U - (reg)])
#define TRAP_PC(frame) ((frame)[16])
#define TRAP_FLAGS(frame) ((frame)[17])
#define TRAP_USER_SP(frame) ((frame)[18])

typedef struct {
    KernelUserImage image;
    uint64_t registers[15];
    uint64_t pc;
    uint64_t flags;
    uint64_t stack_pointer;
    uint64_t pid;
    int64_t exit_status;
    uint64_t state;
    uint64_t write_count;
} KernelTask;

static KernelTask scheduler_tasks[KERNEL_TASK_COUNT];
static size_t scheduler_current;
static uint64_t scheduler_timer_count;
static uint64_t scheduler_preemption_count;
static int scheduler_active;

static void scheduler_save(KernelTask *task, const uint64_t *frame)
{
    for (size_t reg = 0; reg < 15; ++reg) {
        task->registers[reg] = TRAP_REGISTER(frame, reg);
    }
    task->pc = TRAP_PC(frame);
    task->flags = TRAP_FLAGS(frame) & UINT64_C(0x3F);
    task->stack_pointer = TRAP_USER_SP(frame);
}

static void scheduler_load(const KernelTask *task, uint64_t *frame)
{
    for (size_t reg = 0; reg < 15; ++reg) {
        TRAP_REGISTER(frame, reg) = task->registers[reg];
    }
    TRAP_PC(frame) = task->pc;
    TRAP_FLAGS(frame) = task->flags | KERNEL_SAVED_USER_MODE |
                        KERNEL_CPU_FLAG_INTERRUPT_ENABLE;
    TRAP_USER_SP(frame) = task->stack_pointer;
    cvm_set_ptbr((uint64_t)kernel_address_space_root(
        task->image.address_space));
}

static size_t scheduler_next_runnable(void)
{
    for (size_t offset = 1; offset <= KERNEL_TASK_COUNT; ++offset) {
        size_t candidate = (scheduler_current + offset) % KERNEL_TASK_COUNT;
        if (scheduler_tasks[candidate].state == KERNEL_TASK_RUNNABLE) {
            return candidate;
        }
    }
    return KERNEL_TASK_COUNT;
}

static void scheduler_switch(uint64_t *frame, int timer_switch)
{
    size_t next = scheduler_next_runnable();
    if (next == KERNEL_TASK_COUNT) return;
    if (next != scheduler_current) {
        scheduler_current = next;
        if (timer_switch) ++scheduler_preemption_count;
    }
    scheduler_load(&scheduler_tasks[scheduler_current], frame);
}

static void scheduler_finish(void)
{
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_CONTROL_OFFSET, 0);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                     IRQ_CONTROLLER_ENABLE_CLEAR_OFFSET,
                     UINT64_C(1) << KERNEL_TIMER_INTERRUPT_LINE);
    int success = scheduler_timer_count != 0 &&
                  scheduler_preemption_count != 0;
    for (size_t i = 0; i < KERNEL_TASK_COUNT; ++i) {
        if (scheduler_tasks[i].state != KERNEL_TASK_EXITED ||
            scheduler_tasks[i].exit_status != 0 ||
            scheduler_tasks[i].write_count == 0) {
            success = 0;
        }
    }
    if (success) {
        kernel_uart_puts("KERNEL: USER SYSCALL OK\n");
        kernel_uart_puts("KERNEL: PREEMPTIVE SCHEDULER OK\n");
        kernel_uart_puts("KERNEL: READY\n");
    } else {
        kernel_uart_puts("KERNEL ERROR: scheduler runtime self-test\n");
    }
    cvm_halt();
}

KernelAddressSpace *kernel_scheduler_current_space(void)
{
    if (!scheduler_active || scheduler_current >= KERNEL_TASK_COUNT ||
        scheduler_tasks[scheduler_current].state != KERNEL_TASK_RUNNABLE) {
        return NULL;
    }
    return scheduler_tasks[scheduler_current].image.address_space;
}

uint64_t kernel_scheduler_current_pid(void)
{
    return kernel_scheduler_current_space() != NULL
               ? scheduler_tasks[scheduler_current].pid : 0;
}

void kernel_scheduler_note_write(void)
{
    if (kernel_scheduler_current_space() != NULL) {
        ++scheduler_tasks[scheduler_current].write_count;
    }
}

void kernel_scheduler_yield(uint64_t *frame)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    scheduler_save(&scheduler_tasks[scheduler_current], frame);
    scheduler_switch(frame, 0);
}

void kernel_scheduler_exit(uint64_t *frame, int64_t status)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    KernelTask *task = &scheduler_tasks[scheduler_current];
    scheduler_save(task, frame);
    task->exit_status = status;
    task->state = KERNEL_TASK_EXITED;
    size_t next = scheduler_next_runnable();
    if (next == KERNEL_TASK_COUNT) scheduler_finish();
    scheduler_current = next;
    scheduler_load(&scheduler_tasks[scheduler_current], frame);
}

void kernel_scheduler_timer(uint64_t *frame)
{
    cvm_mmio_write64((uintptr_t)KERNEL_TIMER_ALIAS + TIMER_STATUS_OFFSET,
                     TIMER_STATUS_PENDING);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS + IRQ_CONTROLLER_EOI_OFFSET,
                     KERNEL_TIMER_INTERRUPT_LINE);
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    ++scheduler_timer_count;
    scheduler_save(&scheduler_tasks[scheduler_current], frame);
    scheduler_switch(frame, 1);
}

static int scheduler_load_task(size_t index,
                               const char *message,
                               uint32_t loop_count)
{
    size_t image_size;
    uint8_t *data = kernel_user_test_program_create(
        message, loop_count, &image_size);
    if (data == NULL) return 0;
    KernelTask *task = &scheduler_tasks[index];
    int loaded = kernel_user_image_load(data, image_size, &task->image) == 0;
    kernel_free(data);
    if (!loaded) return 0;
    for (size_t reg = 0; reg < 15; ++reg) task->registers[reg] = 0;
    task->pc = task->image.entry;
    task->flags = KERNEL_CPU_FLAG_INTERRUPT_ENABLE;
    task->stack_pointer = task->image.stack_pointer;
    task->pid = index + 1;
    task->exit_status = -1;
    task->state = KERNEL_TASK_RUNNABLE;
    task->write_count = 0;
    return 1;
}

int kernel_scheduler_self_test(void)
{
    scheduler_active = 0;
    scheduler_timer_count = 0;
    scheduler_preemption_count = 0;
    scheduler_current = 0;
    for (size_t i = 0; i < KERNEL_TASK_COUNT; ++i) {
        scheduler_tasks[i].state = 0;
        scheduler_tasks[i].image.address_space = NULL;
    }
    if (!scheduler_load_task(0, "USER[1]: syscall write\n", 1000000) ||
        !scheduler_load_task(1, "USER[2]: syscall write\n", 1000000)) {
        for (size_t i = 0; i < KERNEL_TASK_COUNT; ++i) {
            kernel_user_image_destroy(&scheduler_tasks[i].image);
        }
        return 1;
    }

    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                     IRQ_CONTROLLER_ENABLE_SET_OFFSET,
                     UINT64_C(1) << KERNEL_TIMER_INTERRUPT_LINE);
    if (cvm_mmio_read64((uintptr_t)KERNEL_IRQ_ALIAS +
                        IRQ_CONTROLLER_RESULT_OFFSET) !=
        IRQ_CONTROLLER_RESULT_SUCCESS) {
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
    scheduler_active = 1;
    kernel_uart_puts("KERNEL: USER TASKS START\n");
    kernel_start_user(
        kernel_address_space_root(scheduler_tasks[0].image.address_space),
        scheduler_tasks[0].pc,
        scheduler_tasks[0].stack_pointer);
    return 1;
}
