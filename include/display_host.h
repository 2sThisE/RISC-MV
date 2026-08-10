#ifndef DISPLAY_HOST_H
#define DISPLAY_HOST_H

#include <stdint.h>

#define VM_DISPLAY_SERVICE_NAME "display.present"
#define VM_DISPLAY_HOST_VERSION 1U

#define VM_PIXEL_FORMAT_XRGB8888 1U

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    void *context;
    int (*resize)(void *context, uint32_t width, uint32_t height);
    int (*present)(void *context,
                   const void *pixels,
                   uint32_t width,
                   uint32_t height,
                   uint32_t stride,
                   uint32_t format,
                   uint64_t frame_number);
} VmDisplayHost;

#endif
