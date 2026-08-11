#include "boot_format.h"
#include "bus.h"
#include "cpu.h"
#include "mmu.h"
#include "ram.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_TEST_RAM_SIZE ((size_t)0x100000)
#define BOOTLOADER_BASE ((size_t)0x20000)
#define BOOT_STATE ((size_t)0x9000)
#define FIRMWARE_TABLE ((size_t)0x6000)
#define TEST_RETURN_ADDRESS ((uint64_t)0x1000)
#define TEST_STACK_ADDRESS ((uint64_t)0x18000)
#define TEST_STAGING_ADDRESS ((uint64_t)0xE0000)

typedef struct {
    uint8_t *memory;
    RAM ram;
    Bus bus;
    CPU cpu;
} BootRegressionFixture;

static void boot_write_u64(uint8_t *memory, size_t offset, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        memory[offset + i] = (uint8_t)(value >> (i * 8));
    }
}

static uint64_t boot_read_u64(const uint8_t *memory, size_t offset)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= (uint64_t)memory[offset + i] << (i * 8);
    }
    return value;
}

static FILE *boot_open_artifact(const char *relative_path)
{
    static const char *const prefixes[] = {"", "..\\..\\"};
    char path[512];
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        int written = snprintf(path,
                               sizeof(path),
                               "%s%s",
                               prefixes[i],
                               relative_path);
        assert(written > 0 && (size_t)written < sizeof(path));
        FILE *file = fopen(path, "rb");
        if (file != NULL) {
            return file;
        }
    }
    return NULL;
}

static size_t boot_load_binary(uint8_t *destination, size_t capacity)
{
    FILE *file = boot_open_artifact("build\\examples\\bootloader.bin");
    assert(file != NULL);
    size_t size = fread(destination, 1, capacity, file);
    assert(!ferror(file));
    assert(size != 0);
    assert(size < capacity);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    return size;
}

static uint64_t boot_symbol_address(const char *wanted)
{
    FILE *file = boot_open_artifact(
        "build\\examples\\sym\\bootloader.sym");
    assert(file != NULL);
    unsigned long long address;
    char name[128];
    while (fscanf(file, "%llx %127s", &address, name) == 2) {
        if (strcmp(name, wanted) == 0) {
            assert(fclose(file) == 0);
            return (uint64_t)address;
        }
    }
    assert(fclose(file) == 0);
    assert(!"bootloader symbol is missing");
    return 0;
}

static void boot_fixture_init(BootRegressionFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->memory = calloc(1, BOOT_TEST_RAM_SIZE);
    assert(fixture->memory != NULL);
    fixture->ram.data = fixture->memory;
    fixture->ram.size = BOOT_TEST_RAM_SIZE;
    assert(bus_init(&fixture->bus, &fixture->ram));
    assert(cpu_init(&fixture->cpu, &fixture->ram));
    (void)boot_load_binary(fixture->memory + BOOTLOADER_BASE,
                           BOOT_TEST_RAM_SIZE - BOOTLOADER_BASE);
    fixture->memory[TEST_RETURN_ADDRESS] = OP_HALT;
    fixture->cpu.registers[REGISTER_SP] = TEST_STACK_ADDRESS;
    boot_write_u64(fixture->memory,
                   (size_t)TEST_STACK_ADDRESS,
                   TEST_RETURN_ADDRESS);
}

static void boot_fixture_destroy(BootRegressionFixture *fixture)
{
    free(fixture->memory);
    *fixture = (BootRegressionFixture){0};
}

static uint64_t boot_call(BootRegressionFixture *fixture,
                          const char *symbol)
{
    fixture->cpu.pc = boot_symbol_address(symbol);
    size_t steps = 0;
    while (!fixture->cpu.halted) {
        assert(steps++ < 1000000);
        assert(cpu_step(&fixture->cpu, &fixture->bus));
    }
    assert(fixture->cpu.registers[REGISTER_SP] ==
           TEST_STACK_ADDRESS + 8);
    return fixture->cpu.registers[0];
}

static void boot_set_staging_span(BootRegressionFixture *fixture,
                                  uint64_t staging,
                                  uint64_t kernel_span)
{
    boot_write_u64(fixture->memory, BOOT_STATE + 0x30, staging);
    boot_write_u64(fixture->memory,
                   (size_t)staging + 0x70,
                   kernel_span);
}

static void boot_encode_map(BootRegressionFixture *fixture,
                            size_t address,
                            const CvmMemoryMapEntry *entries,
                            size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        cvm_memory_map_entry_encode(
            fixture->memory + address + i * CVM_MEMORY_MAP_ENTRY_SIZE,
            &entries[i]);
    }
    fixture->cpu.registers[0] = address;
    fixture->cpu.registers[1] = count;
}

static void test_fragmented_memory_map_first_fit(void)
{
    BootRegressionFixture fixture;
    boot_fixture_init(&fixture);
    boot_set_staging_span(&fixture, TEST_STAGING_ADDRESS, 0x1000);
    const CvmMemoryMapEntry entries[] = {
        {0, 0x30000, CVM_MEMORY_RESERVED, 0, 0},
        {0x30000, 0x800, CVM_MEMORY_USABLE, 0, 0},
        {0x30800, 0x10801, CVM_MEMORY_RESERVED, 0, 0},
        {0x41001, 0x2FFF, CVM_MEMORY_USABLE, 0, 0}
    };
    boot_encode_map(&fixture, 0x8400, entries,
                    sizeof(entries) / sizeof(entries[0]));
    assert(boot_call(&fixture, "select_kernel_physical_base") == 0);
    assert(boot_read_u64(fixture.memory, BOOT_STATE + 0x18) == 0x42000);
    assert(boot_read_u64(fixture.memory, BOOT_STATE + 0x20) == 0x42000);
    boot_fixture_destroy(&fixture);
}

static void test_small_ram_profile_is_rejected(void)
{
    BootRegressionFixture fixture;
    boot_fixture_init(&fixture);
    boot_set_staging_span(&fixture, 0x2F000, 0x1000);
    const CvmMemoryMapEntry entries[] = {
        {0, 0x10000, CVM_MEMORY_FIRMWARE, 0, 0},
        {0x10000, 0x1F000, CVM_MEMORY_USABLE, 0, 0}
    };
    boot_encode_map(&fixture, 0x8400, entries,
                    sizeof(entries) / sizeof(entries[0]));
    assert(boot_call(&fixture, "select_kernel_physical_base") != 0);
    boot_fixture_destroy(&fixture);
}

static void test_staging_collision_and_alignment_boundary(void)
{
    BootRegressionFixture fixture;
    boot_fixture_init(&fixture);
    boot_set_staging_span(&fixture, 0x38000, 0x9000);
    const CvmMemoryMapEntry colliding[] = {
        {0x30000, 0x20000, CVM_MEMORY_USABLE, 0, 0}
    };
    boot_encode_map(&fixture, 0x8400, colliding, 1);
    assert(boot_call(&fixture, "select_kernel_physical_base") != 0);
    boot_fixture_destroy(&fixture);

    boot_fixture_init(&fixture);
    boot_set_staging_span(&fixture, 0x32000, 0x1000);
    const CvmMemoryMapEntry exact_boundary[] = {
        {0x30001, 0x1FFF, CVM_MEMORY_USABLE, 0, 0}
    };
    boot_encode_map(&fixture, 0x8400, exact_boundary, 1);
    assert(boot_call(&fixture, "select_kernel_physical_base") == 0);
    assert(boot_read_u64(fixture.memory, BOOT_STATE + 0x18) == 0x31000);
    boot_fixture_destroy(&fixture);
}

static size_t boot_make_kernel_image(uint8_t *image,
                                     size_t capacity,
                                     uint64_t memory_size,
                                     uint64_t alignment)
{
    static const uint8_t payload[16] = {
        OP_HALT, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15
    };
    const size_t size = CVM_KERNEL_HEADER_SIZE +
                        CVM_KERNEL_SEGMENT_SIZE + sizeof(payload);
    assert(capacity >= size);
    memset(image, 0, size);
    CvmKernelHeader header = {0};
    memcpy(header.magic, RISC_MV_EXF_MAGIC, sizeof(header.magic));
    header.format_major = CVM_KERNEL_FORMAT_MAJOR;
    header.format_minor = CVM_KERNEL_FORMAT_MINOR;
    header.header_size = CVM_KERNEL_HEADER_SIZE;
    header.flags = CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL;
    header.isa_id = RARCH_M64_ISA_ID;
    header.isa_version = CVM_ISA_VERSION;
    header.address_bits = CVM_ADDRESS_BITS;
    header.byte_order = CVM_BYTE_ORDER_LITTLE;
    header.segment_count = 1;
    header.segment_entry_size = CVM_KERNEL_SEGMENT_SIZE;
    header.segment_table_offset = CVM_KERNEL_HEADER_SIZE;
    header.entry_physical_address = 0;
    header.entry_virtual_address = UINT64_C(0x40000000);
    header.virtual_base = UINT64_C(0x40000000);
    header.virtual_size = memory_size;
    header.image_file_size = size;
    cvm_kernel_header_encode(image, &header);

    CvmKernelSegment segment = {
        .type = CVM_SEGMENT_LOAD,
        .flags = CVM_SEGMENT_READ | CVM_SEGMENT_EXECUTE,
        .file_offset = CVM_KERNEL_HEADER_SIZE + CVM_KERNEL_SEGMENT_SIZE,
        .load_address = 0,
        .virtual_address = UINT64_C(0x40000000),
        .file_size = sizeof(payload),
        .memory_size = memory_size,
        .alignment = alignment
    };
    cvm_kernel_segment_encode(image + CVM_KERNEL_HEADER_SIZE, &segment);
    memcpy(image + segment.file_offset, payload, sizeof(payload));
    return size;
}

static void boot_rechecksum_kernel(uint8_t *image, size_t size)
{
    CvmKernelHeader header;
    cvm_kernel_header_decode(image, &header);
    header.payload_crc32 = cvm_crc32(image + CVM_KERNEL_HEADER_SIZE,
                                    size - CVM_KERNEL_HEADER_SIZE);
    header.header_crc32 = 0;
    cvm_kernel_header_encode(image, &header);
    header.header_crc32 = cvm_crc32(image, CVM_KERNEL_HEADER_SIZE);
    cvm_kernel_header_encode(image, &header);
}

static void boot_prepare_kernel_validation(BootRegressionFixture *fixture,
                                           uint64_t physical_base,
                                           uint64_t memory_size,
                                           uint64_t alignment,
                                           size_t *image_size)
{
    *image_size = boot_make_kernel_image(
        fixture->memory + TEST_STAGING_ADDRESS,
        BOOT_TEST_RAM_SIZE - (size_t)TEST_STAGING_ADDRESS,
        memory_size,
        alignment);
    boot_rechecksum_kernel(fixture->memory + TEST_STAGING_ADDRESS,
                           *image_size);
    boot_write_u64(fixture->memory, BOOT_STATE + 0x08, *image_size);
    boot_write_u64(fixture->memory, BOOT_STATE + 0x18, physical_base);
    boot_write_u64(fixture->memory, BOOT_STATE + 0x30,
                   TEST_STAGING_ADDRESS);
}

static void test_bootloader_segment_validation(void)
{
    BootRegressionFixture fixture;
    size_t image_size;
    boot_fixture_init(&fixture);
    boot_prepare_kernel_validation(&fixture,
                                   0x30000,
                                   0x1000,
                                   0x1000,
                                   &image_size);
    assert(boot_call(&fixture, "validate_kernel_image") == 0);
    boot_fixture_destroy(&fixture);

    boot_fixture_init(&fixture);
    boot_prepare_kernel_validation(&fixture,
                                   0x30000,
                                   8,
                                   0x1000,
                                   &image_size);
    assert(boot_call(&fixture, "validate_kernel_image") == 4);
    boot_fixture_destroy(&fixture);

    boot_fixture_init(&fixture);
    boot_prepare_kernel_validation(&fixture,
                                   0x30000,
                                   0x1000,
                                   0x1800,
                                   &image_size);
    assert(boot_call(&fixture, "validate_kernel_image") == 4);
    boot_fixture_destroy(&fixture);

    boot_fixture_init(&fixture);
    boot_prepare_kernel_validation(&fixture,
                                   TEST_STAGING_ADDRESS - 0x1000,
                                   0x2000,
                                   0x1000,
                                   &image_size);
    assert(boot_call(&fixture, "validate_kernel_image") == 4);
    boot_fixture_destroy(&fixture);
}

static void test_initial_mmu_mapping(void)
{
    BootRegressionFixture fixture;
    size_t image_size;
    boot_fixture_init(&fixture);
    boot_prepare_kernel_validation(&fixture,
                                   0x30000,
                                   0x1000,
                                   0x1000,
                                   &image_size);
    assert(boot_call(&fixture, "validate_kernel_image") == 0);

    fixture.cpu = (CPU){0};
    assert(cpu_init(&fixture.cpu, &fixture.ram));
    fixture.cpu.registers[REGISTER_SP] = TEST_STACK_ADDRESS;
    boot_write_u64(fixture.memory,
                   (size_t)TEST_STACK_ADDRESS,
                   TEST_RETURN_ADDRESS);
    boot_write_u64(fixture.memory, FIRMWARE_TABLE + 0x20,
                   BOOT_TEST_RAM_SIZE);
    boot_write_u64(fixture.memory, FIRMWARE_TABLE + 0x78,
                   UINT64_C(0xFFFFFFFFFFFD0000));
    assert(boot_call(&fixture, "build_initial_page_tables") == 0);

    uint64_t root = boot_read_u64(fixture.memory, BOOT_STATE + 0x68);
    uint64_t arena = boot_read_u64(fixture.memory, BOOT_STATE + 0x70);
    uint64_t cursor = boot_read_u64(fixture.memory, BOOT_STATE + 0x78);
    assert(root == arena);
    assert((root & MMU_PAGE_MASK) == 0);
    assert(cursor > root && cursor <= TEST_STAGING_ADDRESS);

    uint64_t physical;
    assert(mmu_translate(&fixture.ram,
                         root,
                         0,
                         UINT64_C(0x40000000),
                         MMU_ACCESS_EXECUTE,
                         &physical) == MMU_RESULT_OK);
    assert(physical == 0x30000);
    assert(mmu_translate(&fixture.ram,
                         root,
                         0,
                         UINT64_C(0x100000000) + 0x54321,
                         MMU_ACCESS_READ,
                         &physical) == MMU_RESULT_OK);
    assert(physical == 0x54321);
    boot_fixture_destroy(&fixture);
}

static void test_initial_page_table_staging_collision(void)
{
    BootRegressionFixture fixture;
    size_t image_size;
    boot_fixture_init(&fixture);
    boot_prepare_kernel_validation(&fixture,
                                   0x30000,
                                   0x1000,
                                   0x1000,
                                   &image_size);
    assert(boot_call(&fixture, "validate_kernel_image") == 0);

    /* Leave room for only one page although the mappings require several. */
    memcpy(fixture.memory + 0x32000,
           fixture.memory + TEST_STAGING_ADDRESS,
           image_size);
    boot_write_u64(fixture.memory, BOOT_STATE + 0x30, 0x32000);
    fixture.cpu = (CPU){0};
    assert(cpu_init(&fixture.cpu, &fixture.ram));
    fixture.cpu.registers[REGISTER_SP] = TEST_STACK_ADDRESS;
    boot_write_u64(fixture.memory,
                   (size_t)TEST_STACK_ADDRESS,
                   TEST_RETURN_ADDRESS);
    boot_write_u64(fixture.memory, FIRMWARE_TABLE + 0x20,
                   BOOT_TEST_RAM_SIZE);
    boot_write_u64(fixture.memory, FIRMWARE_TABLE + 0x78,
                   UINT64_C(0xFFFFFFFFFFFD0000));
    assert(boot_call(&fixture, "build_initial_page_tables") != 0);
    assert(boot_read_u64(fixture.memory, BOOT_STATE + 0x78) <= 0x32000);
    boot_fixture_destroy(&fixture);
}

int test_boot_regression(void)
{
    test_fragmented_memory_map_first_fit();
    test_small_ram_profile_is_rejected();
    test_staging_collision_and_alignment_boundary();
    test_bootloader_segment_validation();
    test_initial_mmu_mapping();
    test_initial_page_table_staging_collision();
    return 0;
}
