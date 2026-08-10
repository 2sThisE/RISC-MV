#ifndef KEYBOARD_INPUT_H
#define KEYBOARD_INPUT_H

#include <stdint.h>
#include <stdatomic.h>

#include "keyboard_host.h"

typedef struct {
    VmKeyboardHost host;
    VmKeyboardEventSink sink;
    void *sink_context;
    uint32_t sequence;
    atomic_flag lock;
} KeyboardInput;

int keyboard_input_init(KeyboardInput *input);
void keyboard_input_destroy(KeyboardInput *input);
VmKeyboardHost *keyboard_input_host(KeyboardInput *input);
int keyboard_input_emit(KeyboardInput *input, uint64_t event);

#endif
