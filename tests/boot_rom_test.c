#include "bus.h"
#include "cpu.h"
#include "mmu.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define TEST_ROM_BASE UINT64_C(0x10000)
#define TEST_MMIO_BASE UINT64_C(0x20000)

typedef struct {
    size_t read_count;
} TestMmio;

static int test_mmio_read(void *context,
                          uint64_t offset,
                          size_t width,
                          uint64_t *value)
{
    TestMmio *device = context;
    if (device == NULL || value == NULL || offset != 0 || width != 1) {
        return 0;
    }
    ++device->read_count;
    *value = OP_HALT;
    return 1;
}

static void write_u64_le(uint8_t *memory, size_t address, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        memory[address + i] = (uint8_t)(value >> (i * 8));
    }
}

static void emit_u32(uint8_t *memory, size_t *cursor, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) {
        memory[(*cursor)++] = (uint8_t)(value >> (i * 8));
    }
}

static void emit_u64(uint8_t *memory, size_t *cursor, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        memory[(*cursor)++] = (uint8_t)(value >> (i * 8));
    }
}

static void test_bus_rom_permissions(void)
{
    uint8_t memory[64] = {0};
    uint8_t rom[16];
    for (size_t i = 0; i < sizeof(rom); ++i) {
        rom[i] = (uint8_t)(i + 1);
    }
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    assert(bus_init(&bus, &ram));
    assert(bus_map_rom(&bus, TEST_ROM_BASE, rom, sizeof(rom)));
    assert(!bus_map_rom(&bus,
                        TEST_ROM_BASE + 0x1000,
                        rom,
                        sizeof(rom)));
    assert(!bus_map_rom(&bus, 32, rom, sizeof(rom)));

    uint64_t value;
    assert(bus_fetch(&bus, TEST_ROM_BASE + 1, 4, &value));
    assert(value == UINT64_C(0x05040302));
    assert(bus_read(&bus, TEST_ROM_BASE + 1, 4, &value));
    assert(value == UINT64_C(0x05040302));
    assert(!bus_write(&bus, TEST_ROM_BASE + 1, 1, 0xFF));
    assert(rom[1] == 2);

    TestMmio mmio = {0};
    BusDevice device = {
        .context = &mmio,
        .read = test_mmio_read
    };
    assert(bus_map_device(&bus, TEST_MMIO_BASE, 16, device));
    assert(!bus_map_device(&bus, TEST_ROM_BASE + 8, 16, device));
    assert(bus_read(&bus, TEST_MMIO_BASE, 1, &value));
    assert(mmio.read_count == 1);
    assert(!bus_fetch(&bus, TEST_MMIO_BASE, 1, &value));
    assert(mmio.read_count == 1);
}

static void test_cpu_executes_from_rom(void)
{
    uint8_t memory[256] = {0};
    uint8_t rom[16] = {0};
    size_t cursor = 0;
    rom[cursor++] = OP_MOVI32U;
    rom[cursor++] = 0;
    emit_u32(rom, &cursor, 42);
    rom[cursor++] = OP_HALT;

    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(bus_map_rom(&bus, TEST_ROM_BASE, rom, cursor));
    assert(cpu_init(&cpu, &ram));
    cpu.pc = TEST_ROM_BASE;
    while (!cpu.halted) {
        assert(cpu_step(&cpu, &bus));
    }
    assert(cpu.registers[0] == 42);
}

static void test_rom_store_fault(void)
{
    uint8_t memory[2048] = {0};
    uint8_t rom[64] = {0};
    size_t cursor = 0;
    rom[cursor++] = OP_MOVI64;
    rom[cursor++] = 0;
    emit_u64(rom, &cursor, TEST_ROM_BASE + 63);
    rom[cursor++] = OP_MOVI32U;
    rom[cursor++] = 1;
    emit_u32(rom, &cursor, 0xAA);
    rom[cursor++] = OP_STORE8;
    rom[cursor++] = 0;
    rom[cursor++] = 1;
    rom[cursor++] = OP_HALT;
    rom[63] = 0x5A;

    const uint64_t vbr = 512;
    const uint64_t handler = 256;
    size_t entry = (size_t)vbr +
                   (CPU_VECTOR_EXCEPTION_BASE + CPU_EXCEPTION_DATA_ACCESS) *
                       CPU_VECTOR_ENTRY_SIZE;
    write_u64_le(memory, entry, handler);
    memory[handler] = OP_HALT;

    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(bus_map_rom(&bus, TEST_ROM_BASE, rom, sizeof(rom)));
    assert(cpu_init(&cpu, &ram));
    assert(cpu_set_vector_base(&cpu, &ram, vbr));
    cpu.pc = TEST_ROM_BASE;
    cpu.registers[REGISTER_SP] = sizeof(memory);

    assert(cpu_step(&cpu, &bus));
    assert(cpu_step(&cpu, &bus));
    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == handler);
    assert(cpu.ecause == CPU_EXCEPTION_DATA_ACCESS);
    assert(cpu.badaddr == TEST_ROM_BASE + 63);
    assert(rom[63] == 0x5A);
}

static void test_mmio_is_not_executable(void)
{
    uint8_t memory[2048] = {0};
    const uint64_t vbr = 512;
    const uint64_t handler = 256;
    size_t entry = (size_t)vbr +
                   (CPU_VECTOR_EXCEPTION_BASE +
                    CPU_EXCEPTION_INSTRUCTION_ACCESS) *
                       CPU_VECTOR_ENTRY_SIZE;
    write_u64_le(memory, entry, handler);
    memory[handler] = OP_HALT;

    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    TestMmio mmio = {0};
    BusDevice device = {
        .context = &mmio,
        .read = test_mmio_read
    };
    assert(bus_init(&bus, &ram));
    assert(bus_map_device(&bus, TEST_MMIO_BASE, 16, device));
    assert(cpu_init(&cpu, &ram));
    assert(cpu_set_vector_base(&cpu, &ram, vbr));
    cpu.pc = TEST_MMIO_BASE;
    cpu.registers[REGISTER_SP] = sizeof(memory);
    assert(cpu_step(&cpu, &bus));
    assert(cpu.pc == handler);
    assert(cpu.ecause == CPU_EXCEPTION_INSTRUCTION_ACCESS);
    assert(cpu.badaddr == TEST_MMIO_BASE);
    assert(mmio.read_count == 0);
}

static void test_mmu_can_execute_rom_mapping(void)
{
    enum {
        TEST_RAM_SIZE = 0x5000,
        ROOT = 0x1000,
        LEVEL1 = 0x2000,
        LEVEL0 = 0x3000,
        VIRTUAL_ROM = 0x4000
    };
    uint8_t memory[TEST_RAM_SIZE] = {0};
    uint8_t rom[1] = {OP_HALT};
    RAM ram = {.data = memory, .size = sizeof(memory)};
    Bus bus;
    CPU cpu;
    assert(bus_init(&bus, &ram));
    assert(bus_map_rom(&bus, TEST_ROM_BASE, rom, sizeof(rom)));
    assert(ram_write(&ram,
                     ROOT,
                     8,
                     LEVEL1 | MMU_PTE_VALID));
    assert(ram_write(&ram,
                     LEVEL1,
                     8,
                     LEVEL0 | MMU_PTE_VALID));
    assert(ram_write(&ram,
                     LEVEL0 + 4 * MMU_TABLE_ENTRY_SIZE,
                     8,
                     TEST_ROM_BASE | MMU_PTE_READ |
                         MMU_PTE_EXECUTE | MMU_PTE_VALID));
    assert(cpu_init(&cpu, &ram));
    cpu.ptbr = ROOT;
    cpu.ptbr_set = 1;
    cpu.mmu_enabled = 1;
    cpu.pc = VIRTUAL_ROM;
    assert(cpu_step(&cpu, &bus));
    assert(cpu.halted);
}

int test_boot_rom(void)
{
    test_bus_rom_permissions();
    test_cpu_executes_from_rom();
    test_rom_store_fault();
    test_mmio_is_not_executable();
    test_mmu_can_execute_rom_mapping();
    return 0;
}
