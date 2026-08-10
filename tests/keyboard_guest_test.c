#include "assembler.h"
#include "bus.h"
#include "device_abi.h"
#include "device_manager.h"
#include "host_thread.h"
#include "keyboard_host.h"
#include "keyboard_input.h"
#include "keyboard_protocol.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

const VmDeviceModule *vm_keyboard_device_module(void);

typedef struct {
    KeyboardInput *input;
    HardwareThread *target;
} KeyboardGuestInjection;

static int inject_escape_when_waiting(void *context)
{
    KeyboardGuestInjection *injection = context;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        int state = atomic_load_explicit(&injection->target->state,
                                         memory_order_acquire);
        if (state == HARDWARE_THREAD_WAITING) {
            return keyboard_input_emit(
                injection->input,
                vm_keyboard_event_make(VM_KEY_ESCAPE, 1, 0, 0, 0));
        }
        if (state == HARDWARE_THREAD_HALTED) {
            return 0;
        }
        host_thread_sleep_milliseconds(1);
    }
    return 0;
}

int test_keyboard_guest(void)
{
    static const char source[] =
        ".entry start\n"
        "start:\n"
        "  MOVI32U SP, 0x7F0\n"
        "  MOVI64 R14, 0xFFFFFFFFFFFC0000\n"
        "  LOAD64O R13, R14, 0x10\n"
        "  MOVI32U R0, 0\n"
        "scan:\n"
        "  CMP R0, R13\n"
        "  BRCC GEU, missing\n"
        "  MOV R1, R0\n"
        "  MOVI32U R2, 0xC0\n"
        "  MUL R1, R2\n"
        "  ADDI32 R1, 0x100\n"
        "  ADD R1, R14\n"
        "  LOAD64O R2, R1, 0\n"
        "  TESTI32 R2, 1\n"
        "  BRCC EQ, next\n"
        "  LOAD64O R2, R1, 8\n"
        "  CMPI32 R2, 7\n"
        "  BRCC EQ, found\n"
        "next:\n"
        "  ADDI32 R0, 1\n"
        "  JUMPREL scan\n"
        "found:\n"
        "  LOAD64O R10, R1, 0x28\n"
        "  LOAD64O R11, R1, 0x38\n"
        "  MOV R3, R11\n"
        "  SHL R3, 3\n"
        "  MOVI32U R4, 0x200\n"
        "  ADD R3, R4\n"
        "  MOVI64 R5, keyboard_irq\n"
        "  STORE64 R3, R5\n"
        "  SETVBR R4\n"
        "  MOVI32U R2, 3\n"
        "  STORE64O R10, R2, 0\n"
        "  EI\n"
        "  WAIT\n"
        "missing:\n"
        "  HALT\n"
        "keyboard_irq:\n"
        "  LOAD64O R0, R10, 0x18\n"
        "  ANDI32 R0, 0xFFFF\n"
        "  MOVI32U R1, 0x500\n"
        "  STORE64 R1, R0\n"
        "  HALT\n"
        ".org 0x200\n"
        "  .space 616, 0xFF\n";

    AssemblyResult assembly;
    AssemblyError error;
    assert(assembler_assemble(source, 1, &assembly, &error));

    uint8_t memory[2048] = {0};
    assert(assembly.size <= sizeof(memory) - assembly.base_address);
    memcpy(memory + assembly.base_address,
           assembly.data,
           assembly.size);
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 1, 1));
    DeviceManager manager;
    assert(device_manager_init(&manager, &bus, &vm.interrupt_router));
    assert(bus_map_device(&bus,
                          VIO_HUB_MMIO_BASE,
                          VIO_HUB_MMIO_SIZE,
                          device_manager_hub_as_bus_device(&manager)));

    KeyboardInput input;
    assert(keyboard_input_init(&input));
    assert(device_manager_register_service(
        &manager,
        VM_KEYBOARD_SERVICE_NAME,
        VM_KEYBOARD_HOST_VERSION,
        keyboard_input_host(&input)));
    size_t keyboard_slot;
    assert(device_manager_attach_module(&manager,
                                        vm_keyboard_device_module(),
                                        "",
                                        &keyboard_slot));

    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    boot->cpu.pc = assembly.entry_address;

    KeyboardGuestInjection injection = {
        .input = &input,
        .target = boot
    };
    HostThread injector;
    assert(host_thread_create(&injector,
                              inject_escape_when_waiting,
                              &injection));
    assert(vm_run(&vm));

    int injection_result = 0;
    assert(host_thread_join(&injector, &injection_result));
    assert(injection_result);
    uint64_t captured_usage;
    assert(ram_read(&ram, 0x500, 8, &captured_usage));
    assert(captured_usage == VM_KEY_ESCAPE);
    assert(boot->cpu.halted);

    device_manager_destroy(&manager);
    keyboard_input_destroy(&input);
    vm_destroy(&vm);
    assembly_result_destroy(&assembly);
    return 0;
}
