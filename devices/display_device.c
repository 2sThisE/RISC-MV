#include "device_abi.h"
#include "display_host.h"
#include "display_protocol.h"

#include <stddef.h>
#include <stdlib.h>

#define DISPLAY_CONTROL_MASK (DISPLAY_CONTROL_ENABLE | \
                              DISPLAY_CONTROL_IRQ_ENABLE)

typedef struct {
    VmDeviceHostApi host;
    VmDeviceResources resources;
    VmDisplayHost *frontend;
    uint64_t control;
    uint64_t width;
    uint64_t height;
    uint64_t stride;
    uint64_t format;
    uint64_t framebuffer;
    uint64_t framebuffer_size;
    uint64_t status;
    uint64_t irq_status;
    uint64_t frame_number;
    uint64_t error_code;
    uint8_t *staging;
    size_t staging_capacity;
} DisplayDevice;

static void display_raise_irq(DisplayDevice *display, uint64_t reason)
{
    display->irq_status |= reason;
    if ((display->control & DISPLAY_CONTROL_IRQ_ENABLE) != 0 &&
        display->host.raise_irq != NULL) {
        (void)display->host.raise_irq(display->host.context,
                                     display->resources.irqs[0]);
    }
}

static void display_set_error(DisplayDevice *display, DisplayError error)
{
    display->status = DISPLAY_STATUS_ERROR;
    display->error_code = (uint64_t)error;
    display_raise_irq(display, DISPLAY_IRQ_ERROR);
}

static int display_required_size(DisplayDevice *display,
                                 size_t *required)
{
    if (display->width == 0 || display->height == 0 ||
        display->width > DISPLAY_MAX_WIDTH ||
        display->height > DISPLAY_MAX_HEIGHT) {
        display_set_error(display, DISPLAY_ERROR_DIMENSIONS);
        return 0;
    }
    if (display->format != VM_PIXEL_FORMAT_XRGB8888) {
        display_set_error(display, DISPLAY_ERROR_FORMAT);
        return 0;
    }

    uint64_t minimum_stride = display->width * UINT64_C(4);
    if (display->stride < minimum_stride ||
        display->stride > UINT32_MAX) {
        display_set_error(display, DISPLAY_ERROR_STRIDE);
        return 0;
    }
    if (display->height > UINT64_MAX / display->stride) {
        display_set_error(display, DISPLAY_ERROR_BUFFER_SIZE);
        return 0;
    }

    uint64_t size = display->stride * display->height;
    if (size > display->framebuffer_size || size > SIZE_MAX) {
        display_set_error(display, DISPLAY_ERROR_BUFFER_SIZE);
        return 0;
    }
    *required = (size_t)size;
    return 1;
}

static int ensure_staging(DisplayDevice *display, size_t required)
{
    if (required <= display->staging_capacity) {
        return 1;
    }
    uint8_t *replacement = realloc(display->staging, required);
    if (replacement == NULL) {
        display_set_error(display, DISPLAY_ERROR_ALLOCATION);
        return 0;
    }
    display->staging = replacement;
    display->staging_capacity = required;
    return 1;
}

static void display_present(DisplayDevice *display)
{
    if ((display->control & DISPLAY_CONTROL_ENABLE) == 0) {
        display_set_error(display, DISPLAY_ERROR_DISABLED);
        return;
    }

    size_t required;
    if (!display_required_size(display, &required) ||
        !ensure_staging(display, required)) {
        return;
    }

    display->status = DISPLAY_STATUS_BUSY;
    display->error_code = DISPLAY_ERROR_NONE;
    if (display->host.dma_read == NULL ||
        !display->host.dma_read(display->host.context,
                                display->framebuffer,
                                display->staging,
                                required)) {
        display_set_error(display, DISPLAY_ERROR_DMA);
        return;
    }

    if (display->frontend->resize != NULL &&
        !display->frontend->resize(display->frontend->context,
                                   (uint32_t)display->width,
                                   (uint32_t)display->height)) {
        display_set_error(display, DISPLAY_ERROR_FRONTEND);
        return;
    }

    uint64_t next_frame = display->frame_number + 1;
    if (display->frontend->present == NULL ||
        !display->frontend->present(display->frontend->context,
                                    display->staging,
                                    (uint32_t)display->width,
                                    (uint32_t)display->height,
                                    (uint32_t)display->stride,
                                    (uint32_t)display->format,
                                    next_frame)) {
        display_set_error(display, DISPLAY_ERROR_FRONTEND);
        return;
    }

    display->frame_number = next_frame;
    display->status = DISPLAY_STATUS_READY;
    display_raise_irq(display, DISPLAY_IRQ_PRESENT_COMPLETE);
}

static int display_create(const VmDeviceHostApi *host,
                          const VmDeviceResources *resources,
                          const char *configuration,
                          void **device_context)
{
    (void)configuration;
    if (host == NULL || resources == NULL || device_context == NULL ||
        host->abi_version != VM_DEVICE_ABI_VERSION ||
        host->struct_size < sizeof(*host) || host->get_service == NULL ||
        resources->struct_size < sizeof(*resources) ||
        resources->bar_count != 1 || resources->irq_count != 1) {
        return 0;
    }

    VmDisplayHost *frontend = host->get_service(
        host->context,
        VM_DISPLAY_SERVICE_NAME,
        VM_DISPLAY_HOST_VERSION);
    if (frontend == NULL ||
        frontend->version != VM_DISPLAY_HOST_VERSION ||
        frontend->struct_size < sizeof(*frontend) ||
        frontend->present == NULL) {
        return 0;
    }

    DisplayDevice *display = malloc(sizeof(*display));
    if (display == NULL) {
        return 0;
    }
    *display = (DisplayDevice){
        .host = *host,
        .resources = *resources,
        .frontend = frontend,
        .format = VM_PIXEL_FORMAT_XRGB8888
    };
    *device_context = display;
    if (display->host.log != NULL) {
        display->host.log(display->host.context,
                          1,
                          "display device attached");
    }
    return 1;
}

static void display_destroy(void *device_context)
{
    DisplayDevice *display = device_context;
    if (display == NULL) {
        return;
    }
    free(display->staging);
    free(display);
}

static int display_read(void *device_context,
                        uint32_t bar,
                        uint64_t offset,
                        uint32_t width,
                        uint64_t *value)
{
    DisplayDevice *display = device_context;
    if (display == NULL || value == NULL || bar != 0 || width != 8) {
        return 0;
    }

    switch (offset) {
        case DISPLAY_CONTROL_OFFSET:
            *value = display->control;
            return 1;
        case DISPLAY_WIDTH_OFFSET:
            *value = display->width;
            return 1;
        case DISPLAY_HEIGHT_OFFSET:
            *value = display->height;
            return 1;
        case DISPLAY_STRIDE_OFFSET:
            *value = display->stride;
            return 1;
        case DISPLAY_FORMAT_OFFSET:
            *value = display->format;
            return 1;
        case DISPLAY_FRAMEBUFFER_OFFSET:
            *value = display->framebuffer;
            return 1;
        case DISPLAY_BUFFER_SIZE_OFFSET:
            *value = display->framebuffer_size;
            return 1;
        case DISPLAY_STATUS_OFFSET:
            *value = display->status;
            return 1;
        case DISPLAY_FRAME_NUMBER_OFFSET:
            *value = display->frame_number;
            return 1;
        case DISPLAY_IRQ_STATUS_OFFSET:
            *value = display->irq_status;
            return 1;
        case DISPLAY_CAPABILITIES_OFFSET:
            *value = DISPLAY_CAP_XRGB8888 |
                     DISPLAY_CAP_EXPLICIT_PRESENT;
            return 1;
        case DISPLAY_ERROR_CODE_OFFSET:
            *value = display->error_code;
            return 1;
        default:
            return 0;
    }
}

static int display_write(void *device_context,
                         uint32_t bar,
                         uint64_t offset,
                         uint32_t width,
                         uint64_t value)
{
    DisplayDevice *display = device_context;
    if (display == NULL || bar != 0 || width != 8) {
        return 0;
    }

    switch (offset) {
        case DISPLAY_CONTROL_OFFSET:
            display->control = value & DISPLAY_CONTROL_MASK;
            if ((display->control & DISPLAY_CONTROL_ENABLE) != 0) {
                display->status = DISPLAY_STATUS_READY;
                display->error_code = DISPLAY_ERROR_NONE;
            } else {
                display->status = 0;
            }
            return 1;
        case DISPLAY_WIDTH_OFFSET:
            display->width = value;
            return 1;
        case DISPLAY_HEIGHT_OFFSET:
            display->height = value;
            return 1;
        case DISPLAY_STRIDE_OFFSET:
            display->stride = value;
            return 1;
        case DISPLAY_FORMAT_OFFSET:
            display->format = value;
            return 1;
        case DISPLAY_FRAMEBUFFER_OFFSET:
            display->framebuffer = value;
            return 1;
        case DISPLAY_BUFFER_SIZE_OFFSET:
            display->framebuffer_size = value;
            return 1;
        case DISPLAY_COMMAND_OFFSET:
            if (value == DISPLAY_COMMAND_PRESENT) {
                display_present(display);
            } else {
                display_set_error(display, DISPLAY_ERROR_COMMAND);
            }
            return 1;
        case DISPLAY_STATUS_OFFSET:
            if ((value & DISPLAY_STATUS_ERROR) != 0) {
                display->status =
                    (display->control & DISPLAY_CONTROL_ENABLE) != 0
                        ? DISPLAY_STATUS_READY
                        : 0;
                display->error_code = DISPLAY_ERROR_NONE;
            }
            return 1;
        case DISPLAY_IRQ_STATUS_OFFSET:
            display->irq_status &= ~(value &
                (DISPLAY_IRQ_PRESENT_COMPLETE | DISPLAY_IRQ_ERROR));
            return 1;
        default:
            return 0;
    }
}

static void display_reset(void *device_context)
{
    DisplayDevice *display = device_context;
    if (display == NULL) {
        return;
    }
    display->control = 0;
    display->width = 0;
    display->height = 0;
    display->stride = 0;
    display->format = VM_PIXEL_FORMAT_XRGB8888;
    display->framebuffer = 0;
    display->framebuffer_size = 0;
    display->status = 0;
    display->irq_status = 0;
    display->frame_number = 0;
    display->error_code = DISPLAY_ERROR_NONE;
}

static const VmDeviceModule DISPLAY_MODULE = {
    .abi_version = VM_DEVICE_ABI_VERSION,
    .struct_size = sizeof(VmDeviceModule),
    .descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .name = "framebuffer-display",
        .device_class = VM_DEVICE_CLASS_DISPLAY,
        .vendor_id = UINT32_C(0x564D),
        .device_id = UINT32_C(0x1000),
        .device_version = 1,
        .features = DISPLAY_CAP_XRGB8888 |
                    DISPLAY_CAP_EXPLICIT_PRESENT,
        .bar_count = 1,
        .irq_count = 1,
        .bar_sizes = { DISPLAY_MMIO_SIZE },
        .bar_alignments = { UINT64_C(4096) }
    },
    .create = display_create,
    .destroy = display_destroy,
    .read = display_read,
    .write = display_write,
    .tick = NULL,
    .reset = display_reset
};

#ifdef _WIN32
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const VmDeviceModule *vm_device_query(uint32_t host_abi_version)
{
    return host_abi_version == VM_DEVICE_ABI_VERSION
               ? &DISPLAY_MODULE
               : NULL;
}
