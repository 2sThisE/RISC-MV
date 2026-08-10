#include "bus.h"
#include "device_abi.h"
#include "device_manager.h"
#include "interrupt.h"
#include "keyboard_host.h"
#include "keyboard_input.h"
#include "keyboard_protocol.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>

const VmDeviceModule *vm_keyboard_device_module(void);

int test_keyboard(void)
{
    uint8_t memory[64] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    InterruptController controller;
    interrupt_controller_init(&controller);
    InterruptController *targets[1] = { &controller };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 1));

    DeviceManager manager;
    assert(device_manager_init(&manager, &bus, &router));
    KeyboardInput input;
    assert(keyboard_input_init(&input));
    assert(device_manager_register_service(
        &manager,
        VM_KEYBOARD_SERVICE_NAME,
        VM_KEYBOARD_HOST_VERSION,
        keyboard_input_host(&input)));

    const VmDeviceModule *module = vm_keyboard_device_module();
    assert(module != NULL);
    size_t slot_index;
    assert(device_manager_attach_module(&manager,
                                        module,
                                        "",
                                        &slot_index));
    const DeviceManagerSlot *slot = device_manager_slot(&manager,
                                                        slot_index);
    assert(slot != NULL);
    assert(slot->descriptor.device_class == VM_DEVICE_CLASS_INPUT);
    assert(slot->resources.bar_count == 1);
    assert(slot->resources.irq_count == 1);
    uint64_t base = slot->resources.bar_bases[0];

    /* 하나의 host input service에는 키보드 sink 하나만 연결된다. */
    size_t duplicate_slot;
    assert(!device_manager_attach_module(&manager,
                                         module,
                                         "",
                                         &duplicate_slot));

    uint64_t event = vm_keyboard_event_make(
        VM_KEY_A,
        1,
        0,
        0,
        VM_KEYBOARD_MOD_LEFT_SHIFT);
    assert(keyboard_input_emit(&input, event));
    uint64_t value;
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_EVENT_COUNT_OFFSET,
                    8,
                    &value));
    assert(value == 0);

    assert(bus_write(&bus,
                     base + VM_KEYBOARD_CONTROL_OFFSET,
                     8,
                     VM_KEYBOARD_CONTROL_ENABLE |
                         VM_KEYBOARD_CONTROL_IRQ_ENABLE));
    assert(keyboard_input_emit(&input, event));

    unsigned int line;
    assert(interrupt_controller_take_next(&controller, &line));
    assert(line == slot->resources.irqs[0]);
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VM_KEYBOARD_STATUS_EVENT_AVAILABLE) != 0);
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_IRQ_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VM_KEYBOARD_IRQ_EVENT) != 0);
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_EVENT_DATA_OFFSET,
                    8,
                    &value));
    assert((value & VM_KEYBOARD_EVENT_USAGE_MASK) == VM_KEY_A);
    assert((value & VM_KEYBOARD_EVENT_DOWN) != 0);
    assert(((value & VM_KEYBOARD_EVENT_MODIFIERS_MASK) >>
            VM_KEYBOARD_EVENT_MODIFIERS_SHIFT) ==
           VM_KEYBOARD_MOD_LEFT_SHIFT);
    assert((value & VM_KEYBOARD_EVENT_SEQUENCE_MASK) != 0);

    assert(bus_read(&bus,
                    base + VM_KEYBOARD_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VM_KEYBOARD_STATUS_EVENT_AVAILABLE) == 0);

    for (uint32_t i = 0; i < VM_KEYBOARD_QUEUE_CAPACITY + 1U; ++i) {
        uint16_t usage = (uint16_t)(VM_KEY_A + (i % 26U));
        assert(keyboard_input_emit(
            &input,
            vm_keyboard_event_make(usage, 1, 0, 0, 0)));
    }
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_EVENT_COUNT_OFFSET,
                    8,
                    &value));
    assert(value == VM_KEYBOARD_QUEUE_CAPACITY);
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_DROPPED_OFFSET,
                    8,
                    &value));
    assert(value == 1);
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VM_KEYBOARD_STATUS_OVERFLOW) != 0);
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_IRQ_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & VM_KEYBOARD_IRQ_OVERFLOW) != 0);
    assert(interrupt_controller_take_next(&controller, &line));
    assert(line == slot->resources.irqs[0]);

    assert(bus_write(&bus,
                     base + VM_KEYBOARD_STATUS_OFFSET,
                     8,
                     VM_KEYBOARD_STATUS_OVERFLOW));
    assert(bus_write(&bus,
                     base + VM_KEYBOARD_IRQ_STATUS_OFFSET,
                     8,
                     VM_KEYBOARD_IRQ_OVERFLOW));
    for (uint32_t i = 0; i < VM_KEYBOARD_QUEUE_CAPACITY; ++i) {
        assert(bus_read(&bus,
                        base + VM_KEYBOARD_EVENT_DATA_OFFSET,
                        8,
                        &value));
        assert((value & VM_KEYBOARD_EVENT_USAGE_MASK) != VM_KEY_NONE);
    }
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == 0);
    assert(bus_write(&bus,
                     base + VM_KEYBOARD_DROPPED_OFFSET,
                     8,
                     1));
    assert(bus_read(&bus,
                    base + VM_KEYBOARD_DROPPED_OFFSET,
                    8,
                    &value));
    assert(value == 0);

    assert(device_manager_detach_module(&manager, slot_index));
    assert(!keyboard_input_emit(&input, event));
    device_manager_destroy(&manager);
    keyboard_input_destroy(&input);
    return 0;
}
