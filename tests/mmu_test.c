#include "bus.h"
#include "cpu.h"
#include "mmu.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>

#define TEST_RAM_SIZE 65536U
#define ROOT_TABLE UINT64_C(0xD000)
#define LEVEL1_TABLE UINT64_C(0xE000)
#define LEVEL0_TABLE UINT64_C(0xF000)
#define VECTOR_TABLE UINT64_C(0xA000)

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

static void prepare_page_tables(RAM *ram)
{
    assert(ram_write(ram,
                     ROOT_TABLE,
                     MMU_TABLE_ENTRY_SIZE,
                     LEVEL1_TABLE | MMU_PTE_VALID));
    assert(ram_write(ram,
                     LEVEL1_TABLE,
                     MMU_TABLE_ENTRY_SIZE,
                     LEVEL0_TABLE | MMU_PTE_VALID));
}

static void map_page(RAM *ram,
                     uint64_t virtual_address,
                     uint64_t physical_address,
                     uint64_t permissions)
{
    assert(virtual_address < UINT64_C(0x200000));
    assert((virtual_address & MMU_PAGE_MASK) == 0);
    assert((physical_address & MMU_PAGE_MASK) == 0);

    uint64_t index = (virtual_address >> MMU_PAGE_SHIFT) & UINT64_C(0x1FF);
    assert(ram_write(ram,
                     LEVEL0_TABLE + index * MMU_TABLE_ENTRY_SIZE,
                     MMU_TABLE_ENTRY_SIZE,
                     physical_address | permissions | MMU_PTE_VALID));
}

static void test_walker_permissions(void)
{
    uint8_t memory[TEST_RAM_SIZE] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    assert(ram_enable_synchronization(&ram));
    prepare_page_tables(&ram);
    map_page(&ram,
             0,
             0,
             MMU_PTE_READ | MMU_PTE_WRITE | MMU_PTE_EXECUTE);
    map_page(&ram,
             0x2000,
             0x3000,
             MMU_PTE_READ | MMU_PTE_WRITE | MMU_PTE_USER);

    uint64_t physical_address;
    assert(mmu_root_valid(&ram, ROOT_TABLE));
    assert(!mmu_root_valid(&ram, ROOT_TABLE + 1));
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         0,
                         0x123,
                         MMU_ACCESS_EXECUTE,
                         &physical_address) == MMU_RESULT_OK);
    assert(physical_address == 0x123);
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         1,
                         0x123,
                         MMU_ACCESS_EXECUTE,
                         &physical_address) ==
           MMU_RESULT_PERMISSION_DENIED);
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         1,
                         0x2456,
                         MMU_ACCESS_WRITE,
                         &physical_address) == MMU_RESULT_OK);
    assert(physical_address == 0x3456);
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         0,
                         0x6000,
                         MMU_ACCESS_READ,
                         &physical_address) == MMU_RESULT_NOT_PRESENT);
}

static void test_partial_physical_page_is_rejected(void)
{
    uint8_t memory[65000] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    assert(ram_enable_synchronization(&ram));
    assert(ram_write(&ram, 0, 8, UINT64_C(0x1000) | MMU_PTE_VALID));
    assert(ram_write(&ram, 0x1000, 8, UINT64_C(0x2000) | MMU_PTE_VALID));
    assert(ram_write(&ram,
                     0x2000,
                     8,
                     UINT64_C(0xF000) | MMU_PTE_VALID |
                         MMU_PTE_READ));

    uint64_t physical_address;
    assert(mmu_translate(&ram,
                         0,
                         0,
                         0,
                         MMU_ACCESS_READ,
                         &physical_address) ==
           MMU_RESULT_MALFORMED_ENTRY);
}

static void test_large_page_leaves(void)
{
    uint8_t memory[TEST_RAM_SIZE] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    assert(ram_enable_synchronization(&ram));

    assert(ram_write(&ram,
                     ROOT_TABLE,
                     MMU_TABLE_ENTRY_SIZE,
                     LEVEL1_TABLE | MMU_PTE_VALID));
    assert(ram_write(&ram,
                     LEVEL1_TABLE + 2 * MMU_TABLE_ENTRY_SIZE,
                     MMU_TABLE_ENTRY_SIZE,
                     UINT64_C(0x200000) | MMU_PTE_VALID |
                         MMU_PTE_READ | MMU_PTE_WRITE));
    assert(ram_write(&ram,
                     ROOT_TABLE + MMU_TABLE_ENTRY_SIZE,
                     MMU_TABLE_ENTRY_SIZE,
                     UINT64_C(0x80000000) | MMU_PTE_VALID |
                         MMU_PTE_READ | MMU_PTE_EXECUTE | MMU_PTE_USER));

    uint64_t physical_address;
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         0,
                         UINT64_C(0x00412345),
                         MMU_ACCESS_WRITE,
                         &physical_address) == MMU_RESULT_OK);
    assert(physical_address == UINT64_C(0x00212345));
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         1,
                         UINT64_C(0x52345678),
                         MMU_ACCESS_EXECUTE,
                         &physical_address) == MMU_RESULT_OK);
    assert(physical_address == UINT64_C(0x92345678));
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         1,
                         UINT64_C(0x00412345),
                         MMU_ACCESS_READ,
                         &physical_address) ==
           MMU_RESULT_PERMISSION_DENIED);
}

static void test_misaligned_large_page_is_rejected(void)
{
    uint8_t memory[TEST_RAM_SIZE] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    assert(ram_enable_synchronization(&ram));
    assert(ram_write(&ram,
                     ROOT_TABLE,
                     MMU_TABLE_ENTRY_SIZE,
                     LEVEL1_TABLE | MMU_PTE_VALID));
    assert(ram_write(&ram,
                     LEVEL1_TABLE,
                     MMU_TABLE_ENTRY_SIZE,
                     UINT64_C(0x201000) | MMU_PTE_VALID |
                         MMU_PTE_READ));

    uint64_t physical_address;
    assert(mmu_translate(&ram,
                         ROOT_TABLE,
                         0,
                         0,
                         MMU_ACCESS_READ,
                         &physical_address) ==
           MMU_RESULT_MALFORMED_ENTRY);
}

static void test_cpu_control_and_cross_page_access(void)
{
    uint8_t memory[TEST_RAM_SIZE] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    prepare_page_tables(&ram);
    map_page(&ram,
             0,
             0,
             MMU_PTE_READ | MMU_PTE_WRITE | MMU_PTE_EXECUTE);
    map_page(&ram,
             0x2000,
             0x3000,
             MMU_PTE_READ | MMU_PTE_WRITE);
    map_page(&ram,
             0x3000,
             0x5000,
             MMU_PTE_READ | MMU_PTE_WRITE);
    map_page(&ram,
             0x4000,
             0x7000,
             MMU_PTE_READ | MMU_PTE_WRITE);
    map_page(&ram,
             0x5000,
             0x9000,
             MMU_PTE_READ | MMU_PTE_WRITE);

    assert(ram_write(&ram, 0x3FFC, 4, UINT64_C(0x44332211)));
    assert(ram_write(&ram, 0x5000, 4, UINT64_C(0x88776655)));

    size_t cursor = 1;
    emit_movi64(memory, &cursor, 0, ROOT_TABLE);
    memory[cursor++] = OP_SETPTBR;
    memory[cursor++] = 0;
    memory[cursor++] = OP_GETMMU;
    memory[cursor++] = 1;
    memory[cursor++] = OP_MMUON;
    memory[cursor++] = OP_GETMMU;
    memory[cursor++] = 1;
    emit_movi64(memory, &cursor, 2, 0x2FFC);
    memory[cursor++] = OP_LOAD64;
    memory[cursor++] = 3;
    memory[cursor++] = 2;
    emit_movi64(memory, &cursor, 4, 0x4FFC);
    memory[cursor++] = OP_STORE64;
    memory[cursor++] = 4;
    memory[cursor++] = 3;
    memory[cursor++] = OP_GETPTBR;
    memory[cursor++] = 5;
    memory[cursor++] = OP_MMUOFF;
    memory[cursor++] = OP_GETMMU;
    memory[cursor++] = 6;
    memory[cursor++] = OP_HALT;

    assert(cpu_init(&cpu, &ram));
    assert(!cpu.mmu_enabled);
    assert(!cpu.ptbr_set);
    assert(cpu.epc == 0);
    assert(cpu.ecause == CPU_EXCEPTION_NONE);
    assert(cpu.badaddr == 0);
    assert(cpu.einfo == 0);
    cpu.pc = 1;

    while (!cpu.halted) {
        assert(cpu_step(&cpu, &bus));
    }

    assert(cpu.registers[1] == 1);
    assert(cpu.registers[3] == UINT64_C(0x8877665544332211));
    assert(cpu.registers[5] == ROOT_TABLE);
    assert(cpu.registers[6] == 0);
    assert(cpu.ptbr == ROOT_TABLE);
    assert(cpu.ptbr_set);
    assert(!cpu.mmu_enabled);

    uint64_t value;
    assert(ram_read(&ram, 0x7FFC, 4, &value));
    assert(value == UINT64_C(0x44332211));
    assert(ram_read(&ram, 0x9000, 4, &value));
    assert(value == UINT64_C(0x88776655));
}

static void test_cross_page_instruction_fetch(void)
{
    uint8_t memory[TEST_RAM_SIZE] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    prepare_page_tables(&ram);
    map_page(&ram,
             0,
             0,
             MMU_PTE_READ | MMU_PTE_EXECUTE);
    map_page(&ram,
             0x1000,
             0x2000,
             MMU_PTE_READ | MMU_PTE_EXECUTE);

    memory[0x0FFE] = OP_MOVI64;
    memory[0x0FFF] = 0;
    write_u64_le(&memory[0x2000], UINT64_C(0x0123456789ABCDEF));
    memory[0x2008] = OP_HALT;

    assert(cpu_init(&cpu, &ram));
    cpu.ptbr = ROOT_TABLE;
    cpu.ptbr_set = 1;
    cpu.mmu_enabled = 1;
    cpu.pc = 0x0FFE;

    assert(cpu_step(&cpu, &bus));
    assert(cpu.registers[0] == UINT64_C(0x0123456789ABCDEF));
    assert(cpu.pc == 0x1008);
    assert(cpu_step(&cpu, &bus));
    assert(cpu.halted);
}

static void test_page_fault_delivery(void)
{
    uint8_t memory[TEST_RAM_SIZE] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    prepare_page_tables(&ram);
    map_page(&ram,
             0,
             0,
             MMU_PTE_READ | MMU_PTE_WRITE | MMU_PTE_EXECUTE);
    map_page(&ram,
             0x4000,
             0x6000,
             MMU_PTE_READ | MMU_PTE_WRITE);

    size_t cursor = 1;
    emit_movi64(memory, &cursor, REGISTER_SP, 0x5000);
    emit_movi64(memory, &cursor, 0, ROOT_TABLE);
    memory[cursor++] = OP_SETPTBR;
    memory[cursor++] = 0;
    emit_movi64(memory, &cursor, 1, VECTOR_TABLE);
    memory[cursor++] = OP_SETVBR;
    memory[cursor++] = 1;
    memory[cursor++] = OP_MMUON;
    emit_movi64(memory, &cursor, 2, 0x7000);
    size_t fault_pc = cursor;
    memory[cursor++] = OP_LOAD64;
    memory[cursor++] = 3;
    memory[cursor++] = 2;
    size_t halt_pc = cursor;
    memory[cursor++] = OP_HALT;

    size_t handler = 0x800;
    memory[handler++] = OP_EXCAUSE;
    memory[handler++] = 4;
    memory[handler++] = OP_EXADDR;
    memory[handler++] = 5;
    memory[handler++] = OP_EXPC;
    memory[handler++] = 9;
    memory[handler++] = OP_EXINFO;
    memory[handler++] = 10;
    memory[handler++] = OP_MOV;
    memory[handler++] = 6;
    memory[handler++] = REGISTER_SP;
    memory[handler++] = OP_LOAD64;
    memory[handler++] = 7;
    memory[handler++] = 6;
    emit_movi64(memory, &handler, 8, 3);
    memory[handler++] = OP_ADD;
    memory[handler++] = 7;
    memory[handler++] = 8;
    memory[handler++] = OP_STORE64;
    memory[handler++] = 6;
    memory[handler++] = 7;
    memory[handler++] = OP_IRET;

    assert(ram_write(&ram,
                     VECTOR_TABLE +
                         (CPU_VECTOR_EXCEPTION_BASE +
                          CPU_EXCEPTION_LOAD_PAGE_FAULT) *
                             CPU_VECTOR_ENTRY_SIZE,
                     CPU_VECTOR_ENTRY_SIZE,
                     0x800));

    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    while (!cpu.halted) {
        assert(cpu_step(&cpu, &bus));
    }

    assert(cpu.mmu_enabled);
    assert(cpu.registers[4] == CPU_EXCEPTION_LOAD_PAGE_FAULT);
    assert(cpu.registers[5] == 0x7000);
    assert(cpu.registers[9] == fault_pc);
    assert(cpu.registers[10] ==
           (CPU_EINFO_BADADDR_VALID |
            CPU_EINFO_ACCESS_READ |
            CPU_EINFO_MMU_ENABLED |
            CPU_EINFO_PAGE_NOT_PRESENT));
    assert(cpu.epc == fault_pc);
    assert(cpu.ecause == CPU_EXCEPTION_LOAD_PAGE_FAULT);
    assert(cpu.badaddr == 0x7000);
    assert(cpu.einfo == cpu.registers[10]);
    assert(cpu.registers[REGISTER_SP] == 0x5000);
    assert(fault_pc + 3 == halt_pc);
    assert(!cpu.exception_active);
}

int test_mmu(void)
{
    test_walker_permissions();
    test_partial_physical_page_is_rejected();
    test_large_page_leaves();
    test_misaligned_large_page_is_rejected();
    test_cpu_control_and_cross_page_access();
    test_cross_page_instruction_fetch();
    test_page_fault_delivery();
    return 0;
}
