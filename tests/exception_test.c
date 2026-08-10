#include "bus.h"
#include "cpu.h"
#include "ram.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>

#define VECTOR_TABLE_BASE UINT64_C(0x400)

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static void test_irq_nested_in_exception(void)
{
    uint8_t memory[2048] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    InterruptController interrupts;

    assert(bus_init(&bus, &ram));
    assert(cpu_init(&cpu, &ram));
    interrupt_controller_init(&interrupts);

    memory[1] = 0xFF;
    memory[0x80] = OP_EI;
    memory[0x81] = OP_IRET;
    memory[0x90] = OP_IRET;

    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x90));
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_ILLEGAL_INSTRUCTION) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x80));
    assert(cpu_set_vector_base(&cpu, &ram, VECTOR_TABLE_BASE));

    cpu.pc = 1;
    cpu.registers[REGISTER_SP] = 0x3F0;

    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == 0x80);
    assert(cpu.exception_active);

    assert(cpu_step(&cpu, &bus));
    assert((cpu.flags & CPU_FLAG_INTERRUPT_ENABLE) != 0);
    assert(interrupt_controller_raise(&interrupts, 0));
    assert(cpu_check_interrupt(&cpu, &bus, &interrupts));
    assert(cpu.pc == 0x90);
    assert(cpu.exception_active);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == 0x81);
    assert(cpu.exception_active);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == 1);
    assert(!cpu.exception_active);
    assert(cpu.registers[REGISTER_SP] == 0x3F0);
}

int test_exception(void)
{
    test_irq_nested_in_exception();

    uint8_t memory[2048] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    /* SP와 VBR을 초기화한 뒤 R0=10, R1=0, DIVU R0,R1 */
    memory[1] = OP_MOVI64;
    memory[2] = REGISTER_SP;
    write_u64_le(&memory[3], 0x7F0);
    memory[11] = OP_MOVI64;
    memory[12] = 13;
    write_u64_le(&memory[13], VECTOR_TABLE_BASE);
    memory[21] = OP_SETVBR;
    memory[22] = 13;
    memory[23] = OP_MOVI64;
    memory[24] = 0;
    write_u64_le(&memory[25], 10);
    memory[33] = OP_MOVI64;
    memory[34] = 1;
    write_u64_le(&memory[35], 0);
    memory[43] = OP_DIVU;
    memory[44] = 0;
    memory[45] = 1;

    /* DIV 재실행 후 잘못된 opcode도 예외로 처리한다. */
    memory[46] = 0xFF;
    memory[47] = OP_MOVI64;
    memory[48] = 9;
    write_u64_le(&memory[49], UINT64_MAX);
    memory[57] = OP_LOAD64;
    memory[58] = 10;
    memory[59] = 9;
    memory[60] = OP_HALT;

    /* Divide-by-zero handler: 원인을 읽고 divisor를 2로 고친다. */
    memory[0x80] = OP_EXCAUSE;
    memory[0x81] = 2;
    memory[0x82] = OP_EXADDR;
    memory[0x83] = 3;
    memory[0x84] = OP_EXPC;
    memory[0x85] = 5;
    memory[0x86] = OP_EXINFO;
    memory[0x87] = 14;
    memory[0x88] = OP_MOVI64;
    memory[0x89] = 1;
    write_u64_le(&memory[0x8A], 2);
    memory[0x92] = OP_IRET;

    /* Illegal-instruction handler: 저장된 PC를 1 증가시켜 건너뛴다. */
    memory[0xA0] = OP_EXCAUSE;
    memory[0xA1] = 4;
    memory[0xA2] = OP_MOV;
    memory[0xA3] = 6;
    memory[0xA4] = REGISTER_SP;
    memory[0xA5] = OP_LOAD64;
    memory[0xA6] = 7;
    memory[0xA7] = 6;
    memory[0xA8] = OP_MOVI64;
    memory[0xA9] = 8;
    write_u64_le(&memory[0xAA], 1);
    memory[0xB2] = OP_ADD;
    memory[0xB3] = 7;
    memory[0xB4] = 8;
    memory[0xB5] = OP_STORE64;
    memory[0xB6] = 6;
    memory[0xB7] = 7;
    memory[0xB8] = OP_IRET;

    /* Data-access handler: 잘못된 주소를 유효한 RAM 주소로 고친다. */
    memory[0xC0] = OP_EXCAUSE;
    memory[0xC1] = 11;
    memory[0xC2] = OP_EXADDR;
    memory[0xC3] = 12;
    memory[0xC4] = OP_MOVI64;
    memory[0xC5] = 9;
    write_u64_le(&memory[0xC6], 0x1D0);
    memory[0xCE] = OP_IRET;
    assert(ram_write(&ram, 0x1D0, 8, 123));
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_DIVIDE_BY_ZERO) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x80));
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_ILLEGAL_INSTRUCTION) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0xA0));
    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_DATA_ACCESS) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0xC0));

    VirtualMachine vm;
    assert(vm_init(&vm, &ram, &bus, 1, 1));
    HardwareThread *boot = vm_hardware_thread(&vm, 0, 0);
    assert(boot != NULL);
    boot->cpu.pc = 1;

    assert(vm_run(&vm));
    assert(boot->cpu.registers[0] == 5);
    assert(boot->cpu.registers[2] == CPU_EXCEPTION_DIVIDE_BY_ZERO);
    assert(boot->cpu.registers[3] == 0);
    assert(boot->cpu.registers[5] == 43);
    assert(boot->cpu.registers[14] == 0);
    assert(boot->cpu.registers[4] == CPU_EXCEPTION_ILLEGAL_INSTRUCTION);
    assert(boot->cpu.registers[10] == 123);
    assert(boot->cpu.registers[11] == CPU_EXCEPTION_DATA_ACCESS);
    assert(boot->cpu.registers[12] == UINT64_MAX);
    assert(boot->cpu.registers[REGISTER_SP] == 0x7F0);
    assert(boot->cpu.vbr == VECTOR_TABLE_BASE);
    assert(!boot->cpu.exception_active);
    assert(boot->cpu.halted);

    vm_destroy(&vm);
    return 0;
}
