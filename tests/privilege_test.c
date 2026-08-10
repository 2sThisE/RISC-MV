#include "bus.h"
#include "cpu.h"
#include "interrupt.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>

#define VECTOR_TABLE_BASE UINT64_C(0x400)

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static void configure_cpu(CPU *cpu, RAM *ram)
{
    assert(cpu_init(cpu, ram));
    assert(cpu->mode == CPU_MODE_SUPERVISOR);
    cpu->pc = 1;
    cpu->registers[REGISTER_SP] = 0x300;
    cpu->ksp = 0x3F0;
    assert(cpu_set_vector_base(cpu, ram, VECTOR_TABLE_BASE));
}

static void test_privilege_violation_and_iret(void)
{
    uint8_t memory[2048] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;

    assert(bus_init(&bus, &ram));

    memory[1] = OP_GETMODE;
    memory[2] = 0;
    memory[3] = OP_ENTERUSER;
    memory[4] = OP_GETMODE;
    memory[5] = 1;
    memory[6] = OP_DI;
    memory[7] = OP_GETMODE;
    memory[8] = 7;

    /* 권한 위반 handler: 원인과 handler 모드를 읽고 DI를 건너뛴다. */
    memory[0x80] = OP_EXCAUSE;
    memory[0x81] = 2;
    memory[0x82] = OP_GETMODE;
    memory[0x83] = 3;
    memory[0x84] = OP_MOV;
    memory[0x85] = 4;
    memory[0x86] = REGISTER_SP;
    memory[0x87] = OP_LOAD64;
    memory[0x88] = 5;
    memory[0x89] = 4;
    memory[0x8A] = OP_MOVI64;
    memory[0x8B] = 6;
    write_u64_le(&memory[0x8C], 1);
    memory[0x94] = OP_ADD;
    memory[0x95] = 5;
    memory[0x96] = 6;
    memory[0x97] = OP_STORE64;
    memory[0x98] = 4;
    memory[0x99] = 5;
    memory[0x9A] = OP_IRET;

    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_PRIVILEGE_VIOLATION) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x80));
    configure_cpu(&cpu, &ram);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.registers[0] == CPU_MODE_SUPERVISOR);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.mode == CPU_MODE_USER);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.registers[1] == CPU_MODE_USER);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == 0x80);
    assert(cpu.mode == CPU_MODE_SUPERVISOR);
    assert(cpu.exception_active);
    assert(cpu.epc == 6);
    assert(cpu.ecause == CPU_EXCEPTION_PRIVILEGE_VIOLATION);
    assert(cpu.badaddr == 0);
    assert(cpu.einfo == CPU_EINFO_ORIGIN_USER);

    while (cpu.pc != 7) {
        assert(cpu_step(&cpu, &bus));
    }

    assert(cpu.mode == CPU_MODE_USER);
    assert(!cpu.exception_active);
    assert(cpu.registers[2] == CPU_EXCEPTION_PRIVILEGE_VIOLATION);
    assert(cpu.registers[3] == CPU_MODE_SUPERVISOR);
    assert(cpu.registers[REGISTER_SP] == 0x300);
    assert(cpu.ksp == 0x3F0);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.registers[7] == CPU_MODE_USER);
}

static void test_irq_restores_user_mode(void)
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
    interrupt_controller_init(&interrupts);

    memory[1] = OP_EI;
    memory[2] = OP_ENTERUSER;
    memory[3] = OP_NOP;
    memory[0x80] = OP_GETMODE;
    memory[0x81] = 0;
    memory[0x82] = OP_IRET;

    assert(ram_write(&ram,
                     VECTOR_TABLE_BASE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x80));
    configure_cpu(&cpu, &ram);

    assert(cpu_step(&cpu, &bus));
    assert(cpu_step(&cpu, &bus));
    assert(cpu.mode == CPU_MODE_USER);
    assert((cpu.flags & CPU_FLAG_INTERRUPT_ENABLE) != 0);

    assert(interrupt_controller_raise(&interrupts, 0));
    assert(cpu_check_interrupt(&cpu, &bus, &interrupts));
    assert(cpu.mode == CPU_MODE_SUPERVISOR);
    assert(cpu.pc == 0x80);

    assert(cpu_step(&cpu, &bus));
    assert(cpu.registers[0] == CPU_MODE_SUPERVISOR);
    assert(cpu_step(&cpu, &bus));
    assert(cpu.mode == CPU_MODE_USER);
    assert(cpu.pc == 3);
    assert(cpu.registers[REGISTER_SP] == 0x300);
    assert(cpu.ksp == 0x3F0);
}

int test_privilege(void)
{
    test_privilege_violation_and_iret();
    test_irq_restores_user_mode();
    return 0;
}
