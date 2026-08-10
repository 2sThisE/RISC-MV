#include "keyboard_input.h"

#include "keyboard_protocol.h"

static void input_lock(KeyboardInput *input)
{
    while (atomic_flag_test_and_set_explicit(&input->lock,
                                             memory_order_acquire)) {
    }
}

static void input_unlock(KeyboardInput *input)
{
    atomic_flag_clear_explicit(&input->lock, memory_order_release);
}

static int set_sink(void *context,
                    VmKeyboardEventSink sink,
                    void *sink_context)
{
    KeyboardInput *input = context;
    if (input == NULL || sink == NULL) {
        return 0;
    }
    input_lock(input);
    if (input->sink != NULL) {
        input_unlock(input);
        return 0;
    }
    input->sink = sink;
    input->sink_context = sink_context;
    input_unlock(input);
    return 1;
}

static int clear_sink(void *context,
                      VmKeyboardEventSink sink,
                      void *sink_context)
{
    KeyboardInput *input = context;
    if (input == NULL || sink == NULL) {
        return 0;
    }
    input_lock(input);
    if (input->sink != sink || input->sink_context != sink_context) {
        input_unlock(input);
        return 0;
    }
    input->sink = NULL;
    input->sink_context = NULL;
    input_unlock(input);
    return 1;
}

int keyboard_input_init(KeyboardInput *input)
{
    if (input == NULL) {
        return 0;
    }
    *input = (KeyboardInput){0};
    atomic_flag_clear(&input->lock);
    input->host = (VmKeyboardHost){
        .version = VM_KEYBOARD_HOST_VERSION,
        .struct_size = sizeof(VmKeyboardHost),
        .context = input,
        .set_sink = set_sink,
        .clear_sink = clear_sink
    };
    return 1;
}

void keyboard_input_destroy(KeyboardInput *input)
{
    if (input == NULL) {
        return;
    }
    input_lock(input);
    input->sink = NULL;
    input->sink_context = NULL;
    input_unlock(input);
    *input = (KeyboardInput){0};
}

VmKeyboardHost *keyboard_input_host(KeyboardInput *input)
{
    return input != NULL ? &input->host : NULL;
}

int keyboard_input_emit(KeyboardInput *input, uint64_t event)
{
    if (input == NULL ||
        (event & VM_KEYBOARD_EVENT_USAGE_MASK) == VM_KEY_NONE) {
        return 0;
    }

    input_lock(input);
    if (input->sink == NULL) {
        input_unlock(input);
        return 0;
    }
    uint32_t sequence = ++input->sequence;
    event &= ~VM_KEYBOARD_EVENT_SEQUENCE_MASK;
    event |= (uint64_t)sequence << VM_KEYBOARD_EVENT_SEQUENCE_SHIFT;
    input->sink(input->sink_context, event);
    input_unlock(input);
    return 1;
}
