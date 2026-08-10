#include "bus.h"
#include "core_control.h"
#include "cpu.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>

#define IPI_LINE (INTERRUPT_IPI_LINE_BASE + 1U)
#define WORKER_ENTRY UINT64_C(0x300)
#define IPI_HANDLER UINT64_C(0x500)
#define VECTOR_TABLE_BASE UINT64_C(0x600)

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static void emit_movi64(uint8_t *memory,
                        size_t *cursor,
                        uint8_t destination,
                        uint64_t value)
{
    memory[(*cursor)++] = OP_MOVI64;
    memory[(*cursor)++] = destination;
    write_u64_le(&memory[*cursor], value);
    *cursor += 8;
}

static void emit_device_write(uint8_t *memory,
                              size_t *cursor,
                              uint64_t offset,
                              uint64_t value)
{
    emit_movi64(memory,
                cursor,
                0,
                CORE_CONTROL_MMIO_BASE + offset);
    emit_movi64(memory, cursor, 1, value);
    memory[(*cursor)++] = OP_STORE64;
    memory[(*cursor)++] = 0;
    memory[(*cursor)++] = 1;
}

int test_ipi(void)
{
    uint8_t memory[4096] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 2, 1));
    CoreControlDevice core_control;
    assert(core_control_device_init(&core_control, &vm));
    assert(bus_map_device(&bus,
                          CORE_CONTROL_MMIO_BASE,
                          CORE_CONTROL_MMIO_SIZE,
                          core_control_device_as_bus_device(&core_control)));

    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         IPI_LINE * CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     IPI_HANDLER));

    size_t cursor = 1;
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_TARGET_CORE_OFFSET,
                      1);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_TARGET_THREAD_OFFSET,
                      0);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_ENTRY_PC_OFFSET,
                      WORKER_ENTRY);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_COMMAND_OFFSET,
                      CORE_CONTROL_COMMAND_START);

    /* 대상 스레드가 WAITING이 될 때까지 STATUS를 확인한다. */
    emit_movi64(memory,
                &cursor,
                0,
                CORE_CONTROL_MMIO_BASE + CORE_CONTROL_STATUS_OFFSET);
    emit_movi64(memory,
                &cursor,
                3,
                CORE_CONTROL_STATUS_WAITING);
    size_t poll_address = cursor;
    memory[cursor++] = OP_LOAD64;
    memory[cursor++] = 2;
    memory[cursor++] = 0;
    memory[cursor++] = OP_CMP;
    memory[cursor++] = 2;
    memory[cursor++] = 3;
    memory[cursor++] = OP_JNZ;
    write_u64_le(&memory[cursor], poll_address);
    cursor += 8;

    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_IPI_LINE_OFFSET,
                      IPI_LINE);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_COMMAND_OFFSET,
                      CORE_CONTROL_COMMAND_IPI);
    emit_movi64(memory,
                &cursor,
                0,
                CORE_CONTROL_MMIO_BASE + CORE_CONTROL_RESULT_OFFSET);
    memory[cursor++] = OP_LOAD64;
    memory[cursor++] = 7;
    memory[cursor++] = 0;
    memory[cursor++] = OP_HALT;
    assert(cursor < WORKER_ENTRY);

    /* Core 1은 자기 VBR과 스택을 설정하고 IPI를 기다린다. */
    cursor = WORKER_ENTRY;
    emit_movi64(memory, &cursor, REGISTER_SP, 0xF00);
    emit_movi64(memory, &cursor, 0, VECTOR_TABLE_BASE);
    memory[cursor++] = OP_SETVBR;
    memory[cursor++] = 0;
    memory[cursor++] = OP_GETVBR;
    memory[cursor++] = 4;
    memory[cursor++] = OP_EI;
    memory[cursor++] = OP_WAIT;
    emit_movi64(memory, &cursor, 6, 88);
    memory[cursor++] = OP_HALT;

    cursor = IPI_HANDLER;
    emit_movi64(memory, &cursor, 5, 77);
    memory[cursor++] = OP_IRET;

    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    HardwareThread *target = vm_hardware_thread(&vm, 1, 0);
    assert(boot != NULL && target != NULL);
    boot->cpu.pc = 1;

    assert(vm_run(&vm));
    assert(boot->cpu.registers[7] == CORE_CONTROL_RESULT_SUCCESS);
    assert(target->cpu.vbr == VECTOR_TABLE_BASE);
    assert(target->cpu.registers[4] == VECTOR_TABLE_BASE);
    assert(target->cpu.registers[5] == 77);
    assert(target->cpu.registers[6] == 88);
    assert(target->cpu.registers[REGISTER_SP] == 0xF00);
    assert(target->cpu.halted);

    vm_destroy(&vm);
    return 0;
}
