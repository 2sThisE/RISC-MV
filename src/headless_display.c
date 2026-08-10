#include "headless_display.h"

#include <stdlib.h>
#include <string.h>

static void display_lock(HeadlessDisplay *display)
{
    while (atomic_flag_test_and_set_explicit(&display->lock,
                                             memory_order_acquire)) {
    }
}

static void display_unlock(HeadlessDisplay *display)
{
    atomic_flag_clear_explicit(&display->lock, memory_order_release);
}

static int headless_resize(void *context,
                           uint32_t width,
                           uint32_t height)
{
    HeadlessDisplay *display = context;
    return display != NULL && width != 0 && height != 0;
}

static int headless_present(void *context,
                            const void *pixels,
                            uint32_t width,
                            uint32_t height,
                            uint32_t stride,
                            uint32_t format,
                            uint64_t frame_number)
{
    HeadlessDisplay *display = context;
    if (display == NULL || pixels == NULL || width == 0 || height == 0 ||
        format != VM_PIXEL_FORMAT_XRGB8888 ||
        width > UINT32_MAX / 4 || stride < width * 4 ||
        (size_t)height > SIZE_MAX / stride) {
        return 0;
    }

    size_t required = (size_t)stride * height;
    uint8_t *replacement = malloc(required);
    if (replacement == NULL) {
        return 0;
    }
    memcpy(replacement, pixels, required);

    display_lock(display);
    uint8_t *previous = display->frame;
    display->frame = replacement;
    display->frame_size = required;
    display->width = width;
    display->height = height;
    display->stride = stride;
    display->format = format;
    display->frame_number = frame_number;
    display_unlock(display);
    free(previous);
    return 1;
}

int headless_display_init(HeadlessDisplay *display)
{
    if (display == NULL) {
        return 0;
    }
    *display = (HeadlessDisplay){0};
    atomic_flag_clear(&display->lock);
    display->host = (VmDisplayHost){
        .version = VM_DISPLAY_HOST_VERSION,
        .struct_size = sizeof(VmDisplayHost),
        .context = display,
        .resize = headless_resize,
        .present = headless_present
    };
    return 1;
}

void headless_display_destroy(HeadlessDisplay *display)
{
    if (display == NULL) {
        return;
    }
    free(display->frame);
    *display = (HeadlessDisplay){0};
}

VmDisplayHost *headless_display_host(HeadlessDisplay *display)
{
    return display != NULL ? &display->host : NULL;
}

int headless_display_frame_info(HeadlessDisplay *display,
                                size_t *frame_size,
                                uint32_t *width,
                                uint32_t *height,
                                uint32_t *stride,
                                uint32_t *format,
                                uint64_t *frame_number)
{
    if (display == NULL) {
        return 0;
    }
    display_lock(display);
    if (frame_size != NULL) {
        *frame_size = display->frame_size;
    }
    if (width != NULL) {
        *width = display->width;
    }
    if (height != NULL) {
        *height = display->height;
    }
    if (stride != NULL) {
        *stride = display->stride;
    }
    if (format != NULL) {
        *format = display->format;
    }
    if (frame_number != NULL) {
        *frame_number = display->frame_number;
    }
    display_unlock(display);
    return 1;
}

int headless_display_copy_frame(HeadlessDisplay *display,
                                void *buffer,
                                size_t capacity,
                                size_t *copied)
{
    if (display == NULL || copied == NULL) {
        return 0;
    }
    display_lock(display);
    if (display->frame_size > capacity ||
        (display->frame_size != 0 && buffer == NULL)) {
        display_unlock(display);
        return 0;
    }
    if (display->frame_size != 0) {
        memcpy(buffer, display->frame, display->frame_size);
    }
    *copied = display->frame_size;
    display_unlock(display);
    return 1;
}
