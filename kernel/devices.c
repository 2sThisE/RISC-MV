#include "kernel_internal.h"

#include "block_protocol.h"
#include "builtin_device_protocol.h"
#include "device_abi.h"
#include "display_protocol.h"
#include "keyboard_protocol.h"
#include "vio_protocol.h"

#include <cvm/intrin.h>
#include <cvm/mmio.h>

#define KERNEL_DEVICE_SLOT_COUNT 16U
#define KERNEL_BLOCK_WAIT_LIMIT UINT64_C(20000000)
#define KERNEL_BLOCK_TIMEOUT_TICKS UINT64_C(5000)
#define KERNEL_KEYBOARD_BUFFER_CAPACITY 256U

typedef struct {
    int present;
    uint32_t device_class;
    uintptr_t bar;
    uint64_t bar_size;
    uint32_t irq;
} KernelDevice;

static KernelDevice kernel_devices[KERNEL_DEVICE_SLOT_COUNT];
static KernelDevice *kernel_block_device;
static KernelDevice *kernel_keyboard_device;
static KernelDevice *kernel_display_device;
static uintptr_t kernel_block_dma_physical;
static uint8_t *kernel_block_dma_virtual;
static uint64_t kernel_block_sector_capacity;
static int kernel_block_is_read_only;
static KernelSpinLock kernel_block_lock;
static KernelWaitQueue kernel_block_waiters;
static KernelWaitQueue kernel_keyboard_waiters;
static uint8_t kernel_keyboard_buffer[KERNEL_KEYBOARD_BUFFER_CAPACITY];
static size_t kernel_keyboard_head;
static size_t kernel_keyboard_length;
static volatile int kernel_device_interrupts_enabled;
static volatile int kernel_block_request_pending;
static volatile int kernel_block_request_success;
static volatile uint64_t kernel_block_request_error;
static size_t kernel_block_irq_wait_count;
static size_t kernel_block_irq_completion_count;
static uintptr_t kernel_display_buffer_physical;
static uint32_t *kernel_display_buffer_virtual;

static int block_request_begin(void)
{
    if (kernel_block_request_pending) return 0;
    kernel_block_request_success = 0;
    kernel_block_request_error = VM_BLOCK_ERROR_NONE;
    kernel_block_request_pending = 1;
    return 1;
}

static int block_request_complete(uint64_t irq,
                                  uint64_t status,
                                  uint64_t error)
{
    if (!kernel_block_request_pending) return 0;
    kernel_block_request_error = error;
    kernel_block_request_success =
        (irq & VM_BLOCK_IRQ_COMPLETE) != 0 &&
        (irq & VM_BLOCK_IRQ_ERROR) == 0 &&
        (status & VM_BLOCK_STATUS_ERROR) == 0;
    kernel_block_request_pending = 0;
    ++kernel_block_irq_completion_count;
    return 1;
}

static int keyboard_buffer_push(uint8_t byte)
{
    if (kernel_keyboard_length == KERNEL_KEYBOARD_BUFFER_CAPACITY) return 0;
    size_t tail = (kernel_keyboard_head + kernel_keyboard_length) %
                  KERNEL_KEYBOARD_BUFFER_CAPACITY;
    kernel_keyboard_buffer[tail] = byte;
    ++kernel_keyboard_length;
    return 1;
}

static int keyboard_buffer_pop(uint8_t *byte)
{
    if (byte == NULL || kernel_keyboard_length == 0) return 0;
    *byte = kernel_keyboard_buffer[kernel_keyboard_head];
    kernel_keyboard_head = (kernel_keyboard_head + 1U) %
                           KERNEL_KEYBOARD_BUFFER_CAPACITY;
    --kernel_keyboard_length;
    return 1;
}

static int map_mmio_range(uintptr_t alias,
                          uintptr_t physical,
                          uint64_t size)
{
    if (size == 0 || (alias & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        (physical & (uintptr_t)KERNEL_PAGE_MASK) != 0 ||
        size > UINT64_MAX - KERNEL_PAGE_MASK) {
        return 0;
    }
    uint64_t mapped = (size + KERNEL_PAGE_MASK) & KERNEL_PTE_ADDRESS_MASK;
    for (uint64_t offset = 0; offset < mapped; offset += KERNEL_PAGE_SIZE) {
        if (kernel_map_page(kernel_page_table_root(),
                            alias + (uintptr_t)offset,
                            physical + (uintptr_t)offset,
                            KERNEL_PTE_VALID | KERNEL_PTE_READ |
                                KERNEL_PTE_WRITE) != 0) {
            return 0;
        }
    }
    return 1;
}

static int discover_devices(void)
{
    if (!map_mmio_range((uintptr_t)KERNEL_VIO_ALIAS,
                        (uintptr_t)kernel_boot_info->vio_hub_base,
                        VIO_HUB_MMIO_SIZE) ||
        cvm_mmio_read64((uintptr_t)KERNEL_VIO_ALIAS +
                        VIO_HUB_MAGIC_OFFSET) != VIO_HUB_MAGIC ||
        cvm_mmio_read64((uintptr_t)KERNEL_VIO_ALIAS +
                        VIO_HUB_ABI_OFFSET) != VIO_HUB_ABI_VERSION) {
        return 0;
    }
    uint64_t count = cvm_mmio_read64((uintptr_t)KERNEL_VIO_ALIAS +
                                     VIO_HUB_SLOT_COUNT_OFFSET);
    if (count > KERNEL_DEVICE_SLOT_COUNT) count = KERNEL_DEVICE_SLOT_COUNT;
    for (size_t i = 0; i < (size_t)count; ++i) {
        uintptr_t config = (uintptr_t)KERNEL_VIO_ALIAS +
                           VIO_HUB_SLOT_BASE +
                           i * (uintptr_t)VIO_HUB_SLOT_STRIDE;
        uint64_t status = cvm_mmio_read64(config +
                                          VIO_SLOT_STATUS_OFFSET);
        if ((status & VIO_SLOT_STATUS_PRESENT) == 0) continue;
        uint64_t bar_physical = cvm_mmio_read64(
            config + VIO_SLOT_BAR0_BASE_OFFSET);
        uint64_t bar_size = cvm_mmio_read64(
            config + VIO_SLOT_BAR0_SIZE_OFFSET);
        if (bar_physical == 0 || bar_size == 0 ||
            bar_size > KERNEL_EXTERNAL_MMIO_STRIDE) {
            return 0;
        }
        uintptr_t alias = (uintptr_t)KERNEL_EXTERNAL_MMIO_BASE +
                          i * (uintptr_t)KERNEL_EXTERNAL_MMIO_STRIDE;
        if (!map_mmio_range(alias, (uintptr_t)bar_physical, bar_size)) {
            return 0;
        }
        KernelDevice *device = &kernel_devices[i];
        device->present = 1;
        device->device_class = (uint32_t)cvm_mmio_read64(
            config + VIO_SLOT_CLASS_OFFSET);
        device->bar = alias;
        device->bar_size = bar_size;
        device->irq = (uint32_t)cvm_mmio_read64(
            config + VIO_SLOT_IRQ0_OFFSET);
        if (device->device_class == VM_DEVICE_CLASS_STORAGE &&
            kernel_block_device == NULL) {
            kernel_block_device = device;
        } else if (device->device_class == VM_DEVICE_CLASS_INPUT &&
                   kernel_keyboard_device == NULL) {
            kernel_keyboard_device = device;
        } else if (device->device_class == VM_DEVICE_CLASS_DISPLAY &&
                   kernel_display_device == NULL) {
            kernel_display_device = device;
        }
    }
    return 1;
}

static int block_wait(void)
{
    if (kernel_block_device == NULL) return 0;
    if (kernel_device_interrupts_enabled) {
        ++kernel_block_irq_wait_count;
        int64_t wake_result;
        if (kernel_scheduler_block_kernel(&kernel_block_waiters,
                                          KERNEL_BLOCK_TIMEOUT_TICKS,
                                          -KERNEL_ERROR_IO,
                                          &wake_result) != 0 ||
            wake_result != 0) {
            kernel_block_request_success = 0;
            kernel_block_request_error = VM_BLOCK_ERROR_IO;
            return 0;
        }
        return kernel_block_request_success;
    }
    for (uint64_t wait = 0; wait < KERNEL_BLOCK_WAIT_LIMIT; ++wait) {
        uint64_t status = cvm_mmio_read64(
            kernel_block_device->bar + VM_BLOCK_STATUS_OFFSET);
        if ((status & VM_BLOCK_STATUS_ERROR) != 0) return 0;
        if ((status & VM_BLOCK_STATUS_DONE) != 0) return 1;
        cvm_nop();
    }
    return 0;
}

static int block_command(uint64_t command,
                         uint64_t lba,
                         size_t sector_count)
{
    if (kernel_block_device == NULL ||
        sector_count > (size_t)(KERNEL_PAGE_SIZE / VM_BLOCK_SECTOR_SIZE)) {
        return 0;
    }
    if (kernel_device_interrupts_enabled) {
        /* A timed-out command still belongs to the device until its IRQ is
         * acknowledged.  Do not let a late completion satisfy a newer
         * request that happens to reuse the single controller queue. */
        if (kernel_block_request_pending) return 0;
        cvm_mmio_write64(kernel_block_device->bar + VM_BLOCK_IRQ_ACK_OFFSET,
                         VM_BLOCK_IRQ_COMPLETE | VM_BLOCK_IRQ_ERROR);
        if (!block_request_begin()) return 0;
    }
    cvm_mmio_write64(kernel_block_device->bar + VM_BLOCK_LBA_OFFSET, lba);
    cvm_mmio_write64(kernel_block_device->bar +
                     VM_BLOCK_DMA_ADDRESS_OFFSET,
                     kernel_block_dma_physical);
    cvm_mmio_write64(kernel_block_device->bar +
                     VM_BLOCK_SECTOR_COUNT_OFFSET,
                     sector_count);
    cvm_mmio_write64(kernel_block_device->bar + VM_BLOCK_COMMAND_OFFSET,
                     command);
    return block_wait();
}

int kernel_block_read(uint64_t lba, void *buffer, size_t sector_count)
{
    if (buffer == NULL || sector_count == 0 ||
        lba >= kernel_block_sector_capacity ||
        sector_count > kernel_block_sector_capacity - lba ||
        sector_count > (size_t)(KERNEL_PAGE_SIZE / VM_BLOCK_SECTOR_SIZE)) {
        return 0;
    }
    kernel_spin_lock(&kernel_block_lock);
    if (!block_command(VM_BLOCK_COMMAND_READ, lba, sector_count)) {
        kernel_spin_unlock(&kernel_block_lock);
        return 0;
    }
    size_t bytes = sector_count * (size_t)VM_BLOCK_SECTOR_SIZE;
    uint8_t *output = buffer;
    for (size_t i = 0; i < bytes; ++i) output[i] = kernel_block_dma_virtual[i];
    kernel_spin_unlock(&kernel_block_lock);
    return 1;
}

int kernel_block_write(uint64_t lba,
                       const void *buffer,
                       size_t sector_count)
{
    if (buffer == NULL || sector_count == 0 || kernel_block_is_read_only ||
        lba >= kernel_block_sector_capacity ||
        sector_count > kernel_block_sector_capacity - lba ||
        sector_count > (size_t)(KERNEL_PAGE_SIZE / VM_BLOCK_SECTOR_SIZE)) {
        return 0;
    }
    kernel_spin_lock(&kernel_block_lock);
    size_t bytes = sector_count * (size_t)VM_BLOCK_SECTOR_SIZE;
    const uint8_t *input = buffer;
    for (size_t i = 0; i < bytes; ++i) kernel_block_dma_virtual[i] = input[i];
    int okay = block_command(VM_BLOCK_COMMAND_WRITE, lba, sector_count);
    kernel_spin_unlock(&kernel_block_lock);
    return okay;
}

int kernel_block_flush(void)
{
    if (kernel_block_device == NULL) return 0;
    kernel_spin_lock(&kernel_block_lock);
    int okay = block_command(VM_BLOCK_COMMAND_FLUSH, 0, 0);
    kernel_spin_unlock(&kernel_block_lock);
    return okay;
}

uint64_t kernel_block_capacity(void)
{
    return kernel_block_sector_capacity;
}

int kernel_block_read_only(void)
{
    return kernel_block_is_read_only;
}

int kernel_keyboard_poll(uint64_t *event)
{
    if (event == NULL) return 0;
    uint8_t byte;
    if (keyboard_buffer_pop(&byte)) {
        *event = byte;
        return 1;
    }
    if (kernel_keyboard_device == NULL ||
        cvm_mmio_read64(kernel_keyboard_device->bar +
                        VM_KEYBOARD_EVENT_COUNT_OFFSET) == 0) {
        return 0;
    }
    *event = cvm_mmio_read64(kernel_keyboard_device->bar +
                             VM_KEYBOARD_EVENT_DATA_OFFSET);
    return 1;
}

int kernel_keyboard_read(uint64_t *frame,
                         uintptr_t user_buffer,
                         size_t size,
                         int64_t *result)
{
    KernelAddressSpace *space = kernel_scheduler_current_space();
    size_t capacity = size < 64U ? size : 64U;
    if (result == NULL || frame == NULL || space == NULL || capacity == 0 ||
        !kernel_user_buffer_writable(space, user_buffer, capacity)) {
        if (result != NULL) *result = -KERNEL_ERROR_FAULT;
        return 1;
    }
    uint8_t bytes[64];
    size_t count = 0;
    while (count < capacity && keyboard_buffer_pop(&bytes[count])) {
        ++count;
    }
    if (count != 0) {
        if (!kernel_copy_to_user(space, user_buffer, bytes, count)) {
            *result = -KERNEL_ERROR_FAULT;
        } else {
            *result = (int64_t)count;
        }
        return 1;
    }
    if (kernel_scheduler_block_read(frame, &kernel_keyboard_waiters,
                                    user_buffer, capacity) != 0) {
        *result = -KERNEL_ERROR_AGAIN;
        return 1;
    }
    return 0;
}

static void keyboard_deliver_event(uint64_t event)
{
    uint8_t byte = (uint8_t)(event & VM_KEYBOARD_EVENT_USAGE_MASK);
    if (kernel_wait_queue_wake_read_one(&kernel_keyboard_waiters,
                                        &byte, 1) != 0) {
        return;
    }
    (void)keyboard_buffer_push(byte);
}

static void handle_block_interrupt(void)
{
    uintptr_t bar = kernel_block_device->bar;
    uint64_t irq = cvm_mmio_read64(bar + VM_BLOCK_IRQ_STATUS_OFFSET);
    uint64_t status = cvm_mmio_read64(bar + VM_BLOCK_STATUS_OFFSET);
    uint64_t error = cvm_mmio_read64(bar + VM_BLOCK_ERROR_OFFSET);
    cvm_mmio_write64(bar + VM_BLOCK_IRQ_ACK_OFFSET,
                     irq & (VM_BLOCK_IRQ_COMPLETE | VM_BLOCK_IRQ_ERROR));
    if (block_request_complete(irq, status, error)) {
        (void)kernel_wait_queue_wake_one(
            &kernel_block_waiters,
            kernel_block_request_success ? 0 : -KERNEL_ERROR_IO);
    }
}

static void handle_keyboard_interrupt(void)
{
    uintptr_t bar = kernel_keyboard_device->bar;
    uint64_t irq = cvm_mmio_read64(bar + VM_KEYBOARD_IRQ_STATUS_OFFSET);
    while (cvm_mmio_read64(bar + VM_KEYBOARD_EVENT_COUNT_OFFSET) != 0) {
        keyboard_deliver_event(
            cvm_mmio_read64(bar + VM_KEYBOARD_EVENT_DATA_OFFSET));
    }
    cvm_mmio_write64(bar + VM_KEYBOARD_IRQ_STATUS_OFFSET,
                     irq & (VM_KEYBOARD_IRQ_EVENT |
                            VM_KEYBOARD_IRQ_OVERFLOW));
}

void kernel_devices_interrupt(uint64_t *frame)
{
    uint64_t active = cvm_mmio_read64((uintptr_t)KERNEL_IRQ_ALIAS +
                                      IRQ_CONTROLLER_ACTIVE_OFFSET);
    uint32_t line = UINT32_MAX;
    for (uint32_t candidate = 1; candidate < 64U; ++candidate) {
        if ((active & (UINT64_C(1) << candidate)) != 0) {
            line = candidate;
            break;
        }
    }
    if (kernel_block_device != NULL && line == kernel_block_device->irq) {
        handle_block_interrupt();
    } else if (kernel_keyboard_device != NULL &&
               line == kernel_keyboard_device->irq) {
        handle_keyboard_interrupt();
    }
    if (line != UINT32_MAX) {
        cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                         IRQ_CONTROLLER_EOI_OFFSET, line);
    }
    kernel_scheduler_interrupt_return(frame);
}

int kernel_devices_enable_interrupts(void)
{
    if (kernel_block_device == NULL || kernel_keyboard_device == NULL ||
        kernel_block_device->irq >= 64U || kernel_keyboard_device->irq >= 64U ||
        kernel_block_device->irq == kernel_keyboard_device->irq) {
        return 0;
    }
    cvm_mmio_write64(kernel_block_device->bar + VM_BLOCK_IRQ_ACK_OFFSET,
                     VM_BLOCK_IRQ_COMPLETE | VM_BLOCK_IRQ_ERROR);
    cvm_mmio_write64(kernel_keyboard_device->bar +
                     VM_KEYBOARD_IRQ_STATUS_OFFSET,
                     VM_KEYBOARD_IRQ_EVENT | VM_KEYBOARD_IRQ_OVERFLOW);
    cvm_mmio_write64(kernel_block_device->bar + VM_BLOCK_CONTROL_OFFSET,
                     VM_BLOCK_CONTROL_IRQ_ENABLE);
    cvm_mmio_write64(kernel_keyboard_device->bar +
                     VM_KEYBOARD_CONTROL_OFFSET,
                     VM_KEYBOARD_CONTROL_ENABLE |
                         VM_KEYBOARD_CONTROL_IRQ_ENABLE);
    while (cvm_mmio_read64(kernel_keyboard_device->bar +
                           VM_KEYBOARD_EVENT_COUNT_OFFSET) != 0) {
        keyboard_deliver_event(cvm_mmio_read64(
            kernel_keyboard_device->bar + VM_KEYBOARD_EVENT_DATA_OFFSET));
    }
    uint64_t mask = (UINT64_C(1) << kernel_block_device->irq) |
                    (UINT64_C(1) << kernel_keyboard_device->irq);
    cvm_mmio_write64((uintptr_t)KERNEL_IRQ_ALIAS +
                     IRQ_CONTROLLER_ENABLE_SET_OFFSET, mask);
    if (cvm_mmio_read64((uintptr_t)KERNEL_IRQ_ALIAS +
                        IRQ_CONTROLLER_RESULT_OFFSET) !=
        IRQ_CONTROLLER_RESULT_SUCCESS) {
        return 0;
    }
    kernel_device_interrupts_enabled = 1;
    return 1;
}

int kernel_devices_runtime_valid(void)
{
    return kernel_device_interrupts_enabled &&
           !kernel_block_request_pending &&
           kernel_block_irq_wait_count != 0 &&
           kernel_block_irq_wait_count == kernel_block_irq_completion_count;
}

int kernel_display_present_test_pattern(void)
{
    if (kernel_display_device == NULL ||
        kernel_display_buffer_virtual == NULL) return 0;
    const uint32_t width = 32;
    const uint32_t height = 32;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t red = x * 255U / (width - 1);
            uint32_t green = y * 255U / (height - 1);
            uint32_t blue = ((x ^ y) & 1U) != 0 ? 0xFFU : 0x20U;
            kernel_display_buffer_virtual[y * width + x] =
                blue | (green << 8) | (red << 16);
        }
    }
    uintptr_t bar = kernel_display_device->bar;
    cvm_mmio_write64(bar + DISPLAY_WIDTH_OFFSET, width);
    cvm_mmio_write64(bar + DISPLAY_HEIGHT_OFFSET, height);
    cvm_mmio_write64(bar + DISPLAY_STRIDE_OFFSET, width * 4U);
    cvm_mmio_write64(bar + DISPLAY_FORMAT_OFFSET, 1);
    cvm_mmio_write64(bar + DISPLAY_FRAMEBUFFER_OFFSET,
                     kernel_display_buffer_physical);
    cvm_mmio_write64(bar + DISPLAY_BUFFER_SIZE_OFFSET,
                     width * height * 4U);
    cvm_mmio_write64(bar + DISPLAY_CONTROL_OFFSET,
                     DISPLAY_CONTROL_ENABLE);
    cvm_mmio_write64(bar + DISPLAY_COMMAND_OFFSET,
                     DISPLAY_COMMAND_PRESENT);
    return (cvm_mmio_read64(bar + DISPLAY_STATUS_OFFSET) &
            DISPLAY_STATUS_ERROR) == 0;
}

int kernel_devices_init(void)
{
    kernel_spin_init(&kernel_block_lock);
    for (size_t i = 0; i < KERNEL_DEVICE_SLOT_COUNT; ++i) {
        kernel_devices[i].present = 0;
    }
    kernel_block_device = NULL;
    kernel_keyboard_device = NULL;
    kernel_display_device = NULL;
    kernel_device_interrupts_enabled = 0;
    kernel_block_request_pending = 0;
    kernel_block_request_success = 0;
    kernel_block_request_error = VM_BLOCK_ERROR_NONE;
    kernel_block_irq_wait_count = 0;
    kernel_block_irq_completion_count = 0;
    kernel_keyboard_head = 0;
    kernel_keyboard_length = 0;
    kernel_wait_queue_init(&kernel_keyboard_waiters);
    kernel_wait_queue_init(&kernel_block_waiters);
    cvm_mmio_write64((uintptr_t)KERNEL_UART_ALIAS + UART_CONTROL_OFFSET,
                     UART_CONTROL_ENABLE);
    if (!map_mmio_range((uintptr_t)KERNEL_IRQ_ALIAS,
                        (uintptr_t)kernel_boot_info->irq_controller_base,
                        IRQ_CONTROLLER_MMIO_SIZE) ||
        !map_mmio_range((uintptr_t)KERNEL_TIMER_ALIAS,
                        (uintptr_t)kernel_boot_info->timer_base,
                        TIMER_MMIO_SIZE) ||
        cvm_mmio_read64((uintptr_t)KERNEL_IRQ_ALIAS +
                        IRQ_CONTROLLER_MAGIC_OFFSET) !=
            IRQ_CONTROLLER_MAGIC ||
        !discover_devices() || kernel_block_device == NULL ||
        kernel_keyboard_device == NULL || kernel_display_device == NULL ||
        cvm_mmio_read64(kernel_block_device->bar + VM_BLOCK_MAGIC_OFFSET) !=
            VM_BLOCK_MAGIC ||
        cvm_mmio_read64(kernel_block_device->bar + VM_BLOCK_VERSION_OFFSET) !=
            VM_BLOCK_VERSION ||
        cvm_mmio_read64(kernel_block_device->bar +
                        VM_BLOCK_SECTOR_SIZE_OFFSET) !=
            VM_BLOCK_SECTOR_SIZE ||
        cvm_mmio_read64(kernel_keyboard_device->bar +
                        VM_KEYBOARD_VERSION_OFFSET) !=
            VM_KEYBOARD_PROTOCOL_VERSION ||
        (cvm_mmio_read64(kernel_display_device->bar +
                         DISPLAY_CAPABILITIES_OFFSET) &
         DISPLAY_CAP_XRGB8888) == 0) {
        return 1;
    }

    kernel_block_sector_capacity = cvm_mmio_read64(
        kernel_block_device->bar + VM_BLOCK_CAPACITY_OFFSET);
    uint64_t block_features = cvm_mmio_read64(
        kernel_block_device->bar + VM_BLOCK_FEATURES_OFFSET);
    kernel_block_is_read_only =
        (block_features & VM_BLOCK_FEATURE_READ_ONLY) != 0;
    kernel_block_dma_physical = kernel_pmm_alloc_page();
    kernel_display_buffer_physical = kernel_pmm_alloc_page();
    if (kernel_block_dma_physical == 0 ||
        kernel_display_buffer_physical == 0) return 1;
    kernel_block_dma_virtual = kernel_phys_to_virt(kernel_block_dma_physical);
    kernel_display_buffer_virtual = kernel_phys_to_virt(
        kernel_display_buffer_physical);
    if (kernel_block_dma_virtual == NULL ||
        kernel_display_buffer_virtual == NULL) return 1;
    cvm_mmio_write64(kernel_keyboard_device->bar +
                     VM_KEYBOARD_CONTROL_OFFSET,
                     VM_KEYBOARD_CONTROL_ENABLE);
    return 0;
}

int kernel_devices_self_test(void)
{
    /* A timeout must keep ownership of the depth-one device queue until the
     * original command's late completion arrives.  Only then may a new
     * request begin; an error completion must remain distinguishable. */
    int saved_pending = kernel_block_request_pending;
    int saved_success = kernel_block_request_success;
    uint64_t saved_error = kernel_block_request_error;
    size_t saved_completion_count = kernel_block_irq_completion_count;
    kernel_block_request_pending = 0;
    kernel_block_request_success = 0;
    kernel_block_request_error = VM_BLOCK_ERROR_NONE;
    kernel_block_irq_completion_count = 0;
    int block_state_ok =
        block_request_begin() && kernel_block_request_pending &&
        !block_request_begin();
    kernel_block_request_success = 0;
    kernel_block_request_error = VM_BLOCK_ERROR_IO;
    block_state_ok = block_state_ok && kernel_block_request_pending &&
        !block_request_begin() &&
        block_request_complete(VM_BLOCK_IRQ_COMPLETE,
                               VM_BLOCK_STATUS_DONE,
                               VM_BLOCK_ERROR_NONE) &&
        !kernel_block_request_pending && kernel_block_request_success &&
        kernel_block_irq_completion_count == 1 && block_request_begin() &&
        block_request_complete(VM_BLOCK_IRQ_ERROR,
                               VM_BLOCK_STATUS_ERROR,
                               VM_BLOCK_ERROR_IO) &&
        !kernel_block_request_success &&
        kernel_block_request_error == VM_BLOCK_ERROR_IO &&
        kernel_block_irq_completion_count == 2;
    kernel_block_request_pending = saved_pending;
    kernel_block_request_success = saved_success;
    kernel_block_request_error = saved_error;
    kernel_block_irq_completion_count = saved_completion_count;
    if (!block_state_ok) return 1;

    /* Exercise a full ring, overflow rejection, wraparound, and FIFO order.
     * Runtime producers execute in the IRQ path and consumers execute while
     * the trap path has interrupts masked on the current single-core kernel. */
    kernel_keyboard_head = 0;
    kernel_keyboard_length = 0;
    for (size_t i = 0; i < KERNEL_KEYBOARD_BUFFER_CAPACITY; ++i) {
        if (!keyboard_buffer_push((uint8_t)i)) return 1;
    }
    if (keyboard_buffer_push(0xFF)) return 1;
    uint8_t byte;
    for (size_t i = 0; i < KERNEL_KEYBOARD_BUFFER_CAPACITY / 2U; ++i) {
        if (!keyboard_buffer_pop(&byte) || byte != (uint8_t)i) return 1;
    }
    for (size_t i = 0; i < KERNEL_KEYBOARD_BUFFER_CAPACITY / 2U; ++i) {
        if (!keyboard_buffer_push((uint8_t)i)) return 1;
    }
    for (size_t i = KERNEL_KEYBOARD_BUFFER_CAPACITY / 2U;
         i < KERNEL_KEYBOARD_BUFFER_CAPACITY; ++i) {
        if (!keyboard_buffer_pop(&byte) || byte != (uint8_t)i) return 1;
    }
    for (size_t i = 0; i < KERNEL_KEYBOARD_BUFFER_CAPACITY / 2U; ++i) {
        if (!keyboard_buffer_pop(&byte) || byte != (uint8_t)i) return 1;
    }
    if (keyboard_buffer_pop(&byte) || kernel_keyboard_length != 0) return 1;
    kernel_keyboard_head = 0;

    uint8_t sector[512];
    if (!kernel_block_read(1, sector, 1)) return 1;
    static const uint8_t gpt_magic[8] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    for (size_t i = 0; i < 8; ++i) {
        if (sector[i] != gpt_magic[i]) return 1;
    }
    if (!kernel_display_present_test_pattern()) return 1;
    uint64_t event;
    (void)kernel_keyboard_poll(&event);
    return 0;
}
