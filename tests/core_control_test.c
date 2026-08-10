#include "bus.h"
#include "core_control.h"
#include "cpu.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>

#define WORKER_ENTRY UINT64_C(0x100)
#define STOPPED_ENTRY UINT64_C(0x140)
#define COUNTER_ADDRESS UINT64_C(0x180)

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

int test_core_control(void)
{
    uint8_t memory[512] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 3, 1));

    CoreControlDevice core_control;
    assert(core_control_device_init(&core_control, &vm));
    assert(bus_map_device(&bus,
                          CORE_CONTROL_MMIO_BASE,
                          CORE_CONTROL_MMIO_SIZE,
                          core_control_device_as_bus_device(&core_control)));

    size_t cursor = 1;

    /* Core 1 / Thread 0을 작업 프로그램에서 시작한다. */
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

    /* Core 2는 WAIT 상태에 들어가게 한 다음 STOP을 요청한다. */
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_TARGET_CORE_OFFSET,
                      2);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_ENTRY_PC_OFFSET,
                      STOPPED_ENTRY);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_COMMAND_OFFSET,
                      CORE_CONTROL_COMMAND_START);
    emit_device_write(memory,
                      &cursor,
                      CORE_CONTROL_COMMAND_OFFSET,
                      CORE_CONTROL_COMMAND_STOP);

    emit_movi64(memory,
                &cursor,
                0,
                CORE_CONTROL_MMIO_BASE + CORE_CONTROL_RESULT_OFFSET);
    memory[cursor++] = OP_LOAD64;
    memory[cursor++] = 5;
    memory[cursor++] = 0;
    memory[cursor++] = OP_HALT;
    assert(cursor < WORKER_ENTRY);

    /* Core 1: 공유 카운터를 올리고 자기 ID를 남긴다. */
    cursor = WORKER_ENTRY;
    emit_movi64(memory, &cursor, 0, COUNTER_ADDRESS);
    emit_movi64(memory, &cursor, 1, 1);
    memory[cursor++] = OP_ATOMIC_ADD64;
    memory[cursor++] = 0;
    memory[cursor++] = 1;
    memory[cursor++] = 2;
    memory[cursor++] = OP_COREID;
    memory[cursor++] = 3;
    memory[cursor++] = OP_THREADID;
    memory[cursor++] = 4;
    memory[cursor++] = OP_HALT;
    assert(cursor < STOPPED_ENTRY);

    /* Core 2: IRQ가 없으면 WAITING 상태에 머문다. */
    memory[STOPPED_ENTRY] = OP_WAIT;

    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    boot->cpu.pc = 1;
    assert(vm_run(&vm));

    uint64_t counter;
    assert(ram_read(&ram, COUNTER_ADDRESS, 8, &counter));
    assert(counter == 1);
    assert(boot->cpu.registers[5] == CORE_CONTROL_RESULT_SUCCESS);

    HardwareThread *worker = vm_hardware_thread(&vm, 1, 0);
    HardwareThread *stopped = vm_hardware_thread(&vm, 2, 0);
    assert(worker != NULL && stopped != NULL);
    assert(worker->cpu.registers[3] == 1);
    assert(worker->cpu.registers[4] == 0);
    assert(atomic_load(&worker->state) == HARDWARE_THREAD_HALTED);
    assert(atomic_load(&stopped->state) == HARDWARE_THREAD_HALTED);

    uint64_t value;
    assert(bus_read(&bus,
                    CORE_CONTROL_MMIO_BASE + CORE_CONTROL_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == CORE_CONTROL_STATUS_HALTED);

    /* 정지한 Core 2를 초기화하면 다시 OFFLINE 상태가 된다. */
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_COMMAND_OFFSET,
                     8,
                     CORE_CONTROL_COMMAND_RESET));
    assert(bus_read(&bus,
                    CORE_CONTROL_MMIO_BASE + CORE_CONTROL_RESULT_OFFSET,
                    8,
                    &value));
    assert(value == CORE_CONTROL_RESULT_SUCCESS);
    assert(bus_read(&bus,
                    CORE_CONTROL_MMIO_BASE + CORE_CONTROL_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == CORE_CONTROL_STATUS_OFFLINE);
    assert(stopped->cpu.pc == 0);

    /* 잘못된 대상은 VM을 중단하지 않고 RESULT로 보고한다. */
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE +
                         CORE_CONTROL_TARGET_CORE_OFFSET,
                     8,
                     99));
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_COMMAND_OFFSET,
                     8,
                     CORE_CONTROL_COMMAND_START));
    assert(bus_read(&bus,
                    CORE_CONTROL_MMIO_BASE + CORE_CONTROL_RESULT_OFFSET,
                    8,
                    &value));
    assert(value == CORE_CONTROL_RESULT_INVALID_TARGET);

    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE +
                         CORE_CONTROL_TARGET_CORE_OFFSET,
                     8,
                     0));
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_IPI_LINE_OFFSET,
                     8,
                     INTERRUPT_LINE_COUNT));
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_COMMAND_OFFSET,
                     8,
                     CORE_CONTROL_COMMAND_IPI));
    assert(bus_read(&bus,
                    CORE_CONTROL_MMIO_BASE + CORE_CONTROL_RESULT_OFFSET,
                    8,
                    &value));
    assert(value == CORE_CONTROL_RESULT_INVALID_INTERRUPT);
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_IPI_LINE_OFFSET,
                     8,
                     INTERRUPT_IPI_LINE_BASE - 1));
    assert(bus_write(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_COMMAND_OFFSET,
                     8,
                     CORE_CONTROL_COMMAND_IPI));
    assert(bus_read(&bus,
                    CORE_CONTROL_MMIO_BASE + CORE_CONTROL_RESULT_OFFSET,
                    8,
                    &value));
    assert(value == CORE_CONTROL_RESULT_INVALID_INTERRUPT);
    assert(!bus_read(&bus,
                     CORE_CONTROL_MMIO_BASE + CORE_CONTROL_STATUS_OFFSET,
                     4,
                     &value));

    vm_destroy(&vm);
    return 0;
}
