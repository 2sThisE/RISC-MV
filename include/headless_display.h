#ifndef HEADLESS_DISPLAY_H
#define HEADLESS_DISPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "display_host.h"

typedef struct {
    VmDisplayHost host;
    uint8_t *frame;
    size_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frame_number;
    atomic_flag lock;
} HeadlessDisplay;

int headless_display_init(HeadlessDisplay *display);
void headless_display_destroy(HeadlessDisplay *display);
VmDisplayHost *headless_display_host(HeadlessDisplay *display);
int headless_display_frame_info(HeadlessDisplay *display,
                                size_t *frame_size,
                                uint32_t *width,
                                uint32_t *height,
                                uint32_t *stride,
                                uint32_t *format,
                                uint64_t *frame_number);
int headless_display_copy_frame(HeadlessDisplay *display,
                                void *buffer,
                                size_t capacity,
                                size_t *copied);

#endif
