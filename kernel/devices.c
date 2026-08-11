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
static uintptr_t kernel_display_buffer_physical;
static uint32_t *kernel_display_buffer_virtual;

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
        !block_command(VM_BLOCK_COMMAND_READ, lba, sector_count)) {
        return 0;
    }
    size_t bytes = sector_count * (size_t)VM_BLOCK_SECTOR_SIZE;
    uint8_t *output = buffer;
    for (size_t i = 0; i < bytes; ++i) output[i] = kernel_block_dma_virtual[i];
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
    size_t bytes = sector_count * (size_t)VM_BLOCK_SECTOR_SIZE;
    const uint8_t *input = buffer;
    for (size_t i = 0; i < bytes; ++i) kernel_block_dma_virtual[i] = input[i];
    return block_command(VM_BLOCK_COMMAND_WRITE, lba, sector_count);
}

int kernel_block_flush(void)
{
    if (kernel_block_device == NULL) return 0;
    cvm_mmio_write64(kernel_block_device->bar + VM_BLOCK_COMMAND_OFFSET,
                     VM_BLOCK_COMMAND_FLUSH);
    return block_wait();
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
    if (kernel_keyboard_device == NULL || event == NULL ||
        cvm_mmio_read64(kernel_keyboard_device->bar +
                        VM_KEYBOARD_EVENT_COUNT_OFFSET) == 0) {
        return 0;
    }
    *event = cvm_mmio_read64(kernel_keyboard_device->bar +
                             VM_KEYBOARD_EVENT_DATA_OFFSET);
    return 1;
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
    for (size_t i = 0; i < KERNEL_DEVICE_SLOT_COUNT; ++i) {
        kernel_devices[i].present = 0;
    }
    kernel_block_device = NULL;
    kernel_keyboard_device = NULL;
    kernel_display_device = NULL;
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
