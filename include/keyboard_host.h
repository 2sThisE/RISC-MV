#ifndef KEYBOARD_HOST_H
#define KEYBOARD_HOST_H

#include <stdint.h>

#define VM_KEYBOARD_SERVICE_NAME "input.keyboard"
#define VM_KEYBOARD_HOST_VERSION 1U

typedef void (*VmKeyboardEventSink)(void *context, uint64_t event);

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    void *context;
    int (*set_sink)(void *context,
                    VmKeyboardEventSink sink,
                    void *sink_context);
    int (*clear_sink)(void *context,
                      VmKeyboardEventSink sink,
                      void *sink_context);
} VmKeyboardHost;

#endif
