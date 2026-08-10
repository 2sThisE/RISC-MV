#include "cpu.h"
#include "cpu_internal.h"
#include "cpu_vector.h"
#include "mmu.h"

#include <stdio.h>

#define REGISTER_BITS 64U
#define SIGN_BIT (UINT64_C(1) << 63)
#define CPU_FLAG_MASK (CPU_FLAG_ZERO | CPU_FLAG_NEGATIVE | \
                       CPU_FLAG_CARRY | CPU_FLAG_OVERFLOW | \
                       CPU_FLAG_INTERRUPT_ENABLE | \
                       CPU_FLAG_UNORDERED)
/* IRET이 IRQ 프레임과 동기 예외 프레임을 구분하기 위한 저장 전용 비트다. */
#define CPU_SAVED_FLAG_EXCEPTION_FRAME (UINT64_C(1) << 63)
/* 프레임을 만든 시점의 권한 모드를 기록하는 저장 전용 비트다. */
#define CPU_SAVED_FLAG_USER_MODE (UINT64_C(1) << 62)

typedef int (*OpcodeHandler)(CPU *cpu, RAM *ram);

typedef struct {
    uint64_t virtual_address;
    uint64_t physical_address;
    size_t width;
    size_t byte_offset;
} MemorySegment;

static int set_exception(CPU *cpu,
                         CPUException exception,
                         uint64_t badaddr,
                         uint64_t einfo);
static int ram_range_valid(const RAM *ram,
                           uint64_t address,
                           size_t width);

static int require_supervisor(CPU *cpu)
{
    if (cpu->mode == CPU_MODE_SUPERVISOR) {
        return 1;
    }

    return set_exception(cpu,
                         CPU_EXCEPTION_PRIVILEGE_VIOLATION,
                         0,
                         0);
}

int cpu_internal_illegal_instruction(CPU *cpu)
{
    return set_exception(cpu,
                         CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                         0,
                         0);
}

static uint64_t make_saved_flags(const CPU *cpu, int exception_frame)
{
    uint64_t saved_flags = cpu->flags & CPU_FLAG_MASK;
    if (cpu->mode == CPU_MODE_USER) {
        saved_flags |= CPU_SAVED_FLAG_USER_MODE;
    }
    if (exception_frame) {
        saved_flags |= CPU_SAVED_FLAG_EXCEPTION_FRAME;
    }
    return saved_flags;
}

static int set_exception(CPU *cpu,
                         CPUException exception,
                         uint64_t badaddr,
                         uint64_t einfo)
{
    if (!cpu->exception_pending) {
        cpu->exception_pending = 1;
        cpu->epc = cpu->instruction_pc;
        cpu->ecause = (uint64_t)exception;
        cpu->badaddr = badaddr;
        cpu->einfo = einfo;
        if (cpu->mode == CPU_MODE_USER) {
            cpu->einfo |= CPU_EINFO_ORIGIN_USER;
        }
        if (cpu->mmu_enabled) {
            cpu->einfo |= CPU_EINFO_MMU_ENABLED;
        }
    }
    return 0;
}

static uint64_t access_exception_info(MmuAccess access)
{
    switch (access) {
        case MMU_ACCESS_READ:
            return CPU_EINFO_ACCESS_READ;
        case MMU_ACCESS_WRITE:
            return CPU_EINFO_ACCESS_WRITE;
        case MMU_ACCESS_EXECUTE:
            return CPU_EINFO_ACCESS_EXECUTE;
        case MMU_ACCESS_ATOMIC:
            return CPU_EINFO_ACCESS_READ |
                   CPU_EINFO_ACCESS_WRITE |
                   CPU_EINFO_ACCESS_ATOMIC;
    }
    return 0;
}

static uint64_t mmu_result_exception_info(MmuResult result)
{
    switch (result) {
        case MMU_RESULT_OK:
            return 0;
        case MMU_RESULT_INVALID_VIRTUAL_ADDRESS:
            return CPU_EINFO_PAGE_INVALID_VA;
        case MMU_RESULT_INVALID_ROOT:
            return CPU_EINFO_PAGE_INVALID_ROOT;
        case MMU_RESULT_NOT_PRESENT:
            return CPU_EINFO_PAGE_NOT_PRESENT;
        case MMU_RESULT_MALFORMED_ENTRY:
            return CPU_EINFO_PAGE_MALFORMED;
        case MMU_RESULT_PERMISSION_DENIED:
            return CPU_EINFO_PAGE_PERMISSION;
    }
    return 0;
}

static int set_access_exception(CPU *cpu,
                                CPUException exception,
                                uint64_t address,
                                MmuAccess access,
                                MmuResult mmu_result)
{
    return set_exception(cpu,
                         exception,
                         address,
                         CPU_EINFO_BADADDR_VALID |
                             access_exception_info(access) |
                             mmu_result_exception_info(mmu_result));
}

static CPUException page_fault_for_access(MmuAccess access)
{
    if (access == MMU_ACCESS_EXECUTE) {
        return CPU_EXCEPTION_INSTRUCTION_PAGE_FAULT;
    }
    if (access == MMU_ACCESS_READ) {
        return CPU_EXCEPTION_LOAD_PAGE_FAULT;
    }
    return CPU_EXCEPTION_STORE_PAGE_FAULT;
}

static int translate_address(CPU *cpu,
                             RAM *ram,
                             uint64_t virtual_address,
                             MmuAccess access,
                             int report_fault,
                             uint64_t *physical_address)
{
    if (!cpu->mmu_enabled) {
        *physical_address = virtual_address;
        return 1;
    }

    MmuResult result = mmu_translate(
        ram,
        cpu->ptbr,
        cpu->mode == CPU_MODE_USER,
        virtual_address,
        access,
        physical_address);
    if (result == MMU_RESULT_OK) {
        return 1;
    }

    if (report_fault) {
        return set_access_exception(cpu,
                                    page_fault_for_access(access),
                                    virtual_address,
                                    access,
                                    result);
    }
    return 0;
}

static int build_memory_segments(CPU *cpu,
                                 RAM *ram,
                                 uint64_t virtual_address,
                                 size_t width,
                                 MmuAccess access,
                                 CPUException physical_exception,
                                 int report_fault,
                                 MemorySegment segments[2],
                                 size_t *segment_count)
{
    if (width == 0 || width > VECTOR_REGISTER_SIZE ||
        virtual_address > UINT64_MAX - (uint64_t)(width - 1)) {
        if (report_fault) {
            return set_access_exception(
                cpu,
                cpu->mmu_enabled
                    ? page_fault_for_access(access)
                    : physical_exception,
                virtual_address,
                access,
                cpu->mmu_enabled
                    ? MMU_RESULT_INVALID_VIRTUAL_ADDRESS
                    : MMU_RESULT_OK);
        }
        return 0;
    }

    if (!cpu->mmu_enabled) {
        segments[0] = (MemorySegment){
            .virtual_address = virtual_address,
            .physical_address = virtual_address,
            .width = width,
            .byte_offset = 0
        };
        *segment_count = 1;
        return 1;
    }

    size_t remaining = width;
    uint64_t cursor = virtual_address;
    size_t byte_offset = 0;
    size_t count = 0;

    while (remaining != 0) {
        if (count >= 2) {
            return 0;
        }

        size_t page_remaining =
            (size_t)(MMU_PAGE_SIZE - (cursor & MMU_PAGE_MASK));
        size_t chunk = remaining < page_remaining
                           ? remaining
                           : page_remaining;
        uint64_t physical_address;
        if (!translate_address(cpu,
                               ram,
                               cursor,
                               access,
                               report_fault,
                               &physical_address)) {
            return 0;
        }

        segments[count++] = (MemorySegment){
            .virtual_address = cursor,
            .physical_address = physical_address,
            .width = chunk,
            .byte_offset = byte_offset
        };
        cursor += chunk;
        byte_offset += chunk;
        remaining -= chunk;
    }

    *segment_count = count;
    return 1;
}

static int memory_read(CPU *cpu,
                       RAM *ram,
                       uint64_t virtual_address,
                       size_t width,
                       MmuAccess access,
                       int use_bus,
                       CPUException physical_exception,
                       int report_fault,
                       uint64_t *value)
{
    MemorySegment segments[2];
    size_t segment_count;
    if (value == NULL ||
        !build_memory_segments(cpu,
                               ram,
                               virtual_address,
                               width,
                               access,
                               physical_exception,
                               report_fault,
                               segments,
                               &segment_count)) {
        return 0;
    }

    uint64_t result = 0;
    for (size_t i = 0; i < segment_count; ++i) {
        uint64_t part;
        int success;
        if (access == MMU_ACCESS_EXECUTE) {
            success = cpu->bus != NULL &&
                      bus_fetch(cpu->bus,
                                segments[i].physical_address,
                                segments[i].width,
                                &part);
        } else if (use_bus) {
            success = cpu->bus != NULL &&
                      bus_read(cpu->bus,
                               segments[i].physical_address,
                               segments[i].width,
                               &part);
        } else {
            success = ram_read(ram,
                               segments[i].physical_address,
                               segments[i].width,
                               &part);
        }
        if (!success) {
            if (report_fault) {
                return set_access_exception(cpu,
                                            physical_exception,
                                            segments[i].virtual_address,
                                            access,
                                            MMU_RESULT_OK);
            }
            return 0;
        }
        result |= part << (segments[i].byte_offset * 8);
    }

    *value = result;
    return 1;
}

static int memory_write(CPU *cpu,
                        RAM *ram,
                        uint64_t virtual_address,
                        size_t width,
                        MmuAccess access,
                        int use_bus,
                        CPUException physical_exception,
                        int report_fault,
                        uint64_t value)
{
    MemorySegment segments[2];
    size_t segment_count;
    if (!build_memory_segments(cpu,
                               ram,
                               virtual_address,
                               width,
                               access,
                               physical_exception,
                               report_fault,
                               segments,
                               &segment_count)) {
        return 0;
    }

    if (!use_bus) {
        for (size_t i = 0; i < segment_count; ++i) {
            if (!ram_range_valid(ram,
                                 segments[i].physical_address,
                                 segments[i].width)) {
                if (report_fault) {
                    return set_access_exception(cpu,
                                                physical_exception,
                                                segments[i].virtual_address,
                                                access,
                                                MMU_RESULT_OK);
                }
                return 0;
            }
        }
    }

    for (size_t i = 0; i < segment_count; ++i) {
        uint64_t part = value >> (segments[i].byte_offset * 8);
        int success = use_bus
                          ? cpu->bus != NULL &&
                                bus_write(cpu->bus,
                                          segments[i].physical_address,
                                          segments[i].width,
                                          part)
                          : ram_write(ram,
                                      segments[i].physical_address,
                                      segments[i].width,
                                      part);
        if (!success) {
            if (report_fault) {
                return set_access_exception(cpu,
                                            physical_exception,
                                            segments[i].virtual_address,
                                            access,
                                            MMU_RESULT_OK);
            }
            return 0;
        }
    }

    return 1;
}

int cpu_internal_data_read(CPU *cpu,
                           RAM *ram,
                           uint64_t address,
                           size_t width,
                           uint64_t *value)
{
    return memory_read(cpu,
                       ram,
                       address,
                       width,
                       MMU_ACCESS_READ,
                       1,
                       CPU_EXCEPTION_DATA_ACCESS,
                       1,
                       value);
}

int cpu_internal_data_write(CPU *cpu,
                            RAM *ram,
                            uint64_t address,
                            size_t width,
                            uint64_t value)
{
    return memory_write(cpu,
                        ram,
                        address,
                        width,
                        MMU_ACCESS_WRITE,
                        1,
                        CPU_EXCEPTION_DATA_ACCESS,
                        1,
                        value);
}

static int vector_memory_segments(CPU *cpu,
                                  RAM *ram,
                                  uint64_t address,
                                  MmuAccess access,
                                  MemorySegment segments[2],
                                  size_t *segment_count)
{
    if (!build_memory_segments(cpu,
                               ram,
                               address,
                               VECTOR_REGISTER_SIZE,
                               access,
                               CPU_EXCEPTION_DATA_ACCESS,
                               1,
                               segments,
                               segment_count)) {
        return 0;
    }

    for (size_t i = 0; i < *segment_count; ++i) {
        if (!ram_range_valid(ram,
                             segments[i].physical_address,
                             segments[i].width)) {
            return set_access_exception(cpu,
                                        CPU_EXCEPTION_DATA_ACCESS,
                                        segments[i].virtual_address,
                                        access,
                                        MMU_RESULT_OK);
        }
    }
    return 1;
}

int cpu_internal_vector_read(CPU *cpu,
                             RAM *ram,
                             uint64_t address,
                             uint8_t value[VECTOR_REGISTER_SIZE])
{
    MemorySegment segments[2];
    size_t segment_count;
    if (value == NULL ||
        !vector_memory_segments(cpu,
                                ram,
                                address,
                                MMU_ACCESS_READ,
                                segments,
                                &segment_count)) {
        return 0;
    }

    for (size_t i = 0; i < segment_count; ++i) {
        size_t consumed = 0;
        while (consumed < segments[i].width) {
            size_t chunk = segments[i].width - consumed;
            if (chunk > sizeof(uint64_t)) {
                chunk = sizeof(uint64_t);
            }
            uint64_t part;
            if (!ram_read(ram,
                          segments[i].physical_address + consumed,
                          chunk,
                          &part)) {
                return set_access_exception(
                    cpu,
                    CPU_EXCEPTION_DATA_ACCESS,
                    segments[i].virtual_address + consumed,
                    MMU_ACCESS_READ,
                    MMU_RESULT_OK);
            }
            for (size_t byte = 0; byte < chunk; ++byte) {
                value[segments[i].byte_offset + consumed + byte] =
                    (uint8_t)(part >> (byte * 8));
            }
            consumed += chunk;
        }
    }
    return 1;
}

int cpu_internal_vector_write(CPU *cpu,
                              RAM *ram,
                              uint64_t address,
                              const uint8_t value[VECTOR_REGISTER_SIZE])
{
    MemorySegment segments[2];
    size_t segment_count;
    if (value == NULL ||
        !vector_memory_segments(cpu,
                                ram,
                                address,
                                MMU_ACCESS_WRITE,
                                segments,
                                &segment_count)) {
        return 0;
    }

    for (size_t i = 0; i < segment_count; ++i) {
        size_t consumed = 0;
        while (consumed < segments[i].width) {
            size_t chunk = segments[i].width - consumed;
            if (chunk > sizeof(uint64_t)) {
                chunk = sizeof(uint64_t);
            }
            uint64_t part = 0;
            for (size_t byte = 0; byte < chunk; ++byte) {
                part |= (uint64_t)value[segments[i].byte_offset +
                                       consumed + byte]
                        << (byte * 8);
            }
            if (!ram_write(ram,
                           segments[i].physical_address + consumed,
                           chunk,
                           part)) {
                return set_access_exception(
                    cpu,
                    CPU_EXCEPTION_DATA_ACCESS,
                    segments[i].virtual_address + consumed,
                    MMU_ACCESS_WRITE,
                    MMU_RESULT_OK);
            }
            consumed += chunk;
        }
    }
    return 1;
}

static int ram_write_accessible(CPU *cpu,
                                RAM *ram,
                                uint64_t virtual_address,
                                size_t width,
                                int report_fault)
{
    MemorySegment segments[2];
    size_t segment_count;
    if (!build_memory_segments(cpu,
                               ram,
                               virtual_address,
                               width,
                               MMU_ACCESS_WRITE,
                               CPU_EXCEPTION_STACK_FAULT,
                               report_fault,
                               segments,
                               &segment_count)) {
        return 0;
    }

    for (size_t i = 0; i < segment_count; ++i) {
        if (!ram_range_valid(ram,
                             segments[i].physical_address,
                             segments[i].width)) {
            if (report_fault) {
                return set_access_exception(cpu,
                                            CPU_EXCEPTION_STACK_FAULT,
                                            segments[i].virtual_address,
                                            MMU_ACCESS_WRITE,
                                            MMU_RESULT_OK);
            }
            return 0;
        }
    }
    return 1;
}

static int instruction_address_valid_for_mode(CPU *cpu,
                                              RAM *ram,
                                              uint64_t address,
                                              CPUMode mode,
                                              int report_fault)
{
    uint64_t physical_address = address;
    if (cpu->mmu_enabled) {
        MmuResult result = mmu_translate(ram,
                                         cpu->ptbr,
                                         mode == CPU_MODE_USER,
                                         address,
                                         MMU_ACCESS_EXECUTE,
                                         &physical_address);
        if (result != MMU_RESULT_OK) {
            if (report_fault) {
                return set_access_exception(
                    cpu,
                    CPU_EXCEPTION_INSTRUCTION_PAGE_FAULT,
                    address,
                    MMU_ACCESS_EXECUTE,
                    result);
            }
            return 0;
        }
    }

    uint64_t ignored;
    if (cpu->bus == NULL ||
        !bus_fetch(cpu->bus, physical_address, 1, &ignored)) {
        if (report_fault) {
            return set_access_exception(cpu,
                                        CPU_EXCEPTION_INSTRUCTION_ACCESS,
                                        address,
                                        MMU_ACCESS_EXECUTE,
                                        MMU_RESULT_OK);
        }
        return 0;
    }
    return 1;
}

static int instruction_address_valid(CPU *cpu,
                                     RAM *ram,
                                     uint64_t address)
{
    return instruction_address_valid_for_mode(cpu,
                                              ram,
                                              address,
                                              cpu->mode,
                                              1);
}

static int fetch_u8(CPU *cpu, RAM *ram, uint8_t *result)
{
    uint64_t value;
    if (!memory_read(cpu,
                     ram,
                     cpu->pc,
                     1,
                     MMU_ACCESS_EXECUTE,
                     0,
                     CPU_EXCEPTION_INSTRUCTION_ACCESS,
                     1,
                     &value)) {
        return 0;
    }

    *result = (uint8_t)value;
    cpu->pc++;
    return 1;
}

int cpu_internal_fetch_u8(CPU *cpu, RAM *ram, uint8_t *result)
{
    return fetch_u8(cpu, ram, result);
}

static int fetch_u64_le(CPU *cpu, RAM *ram, uint64_t *result)
{
    if (!memory_read(cpu,
                     ram,
                     cpu->pc,
                     8,
                     MMU_ACCESS_EXECUTE,
                     0,
                     CPU_EXCEPTION_INSTRUCTION_ACCESS,
                     1,
                     result)) {
        return 0;
    }
    cpu->pc += 8;
    return 1;
}

static int fetch_u32_le(CPU *cpu, RAM *ram, uint32_t *result)
{
    uint64_t value;
    if (result == NULL ||
        !memory_read(cpu,
                     ram,
                     cpu->pc,
                     4,
                     MMU_ACCESS_EXECUTE,
                     0,
                     CPU_EXCEPTION_INSTRUCTION_ACCESS,
                     1,
                     &value)) {
        return 0;
    }
    cpu->pc += 4;
    *result = (uint32_t)value;
    return 1;
}

static int register_valid(CPU *cpu, uint8_t reg)
{
    if (reg < REGISTER_COUNT) {
        return 1;
    }

    return set_exception(cpu,
                         CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                         0,
                         0);
}

static int fetch_register(CPU *cpu, RAM *ram, uint8_t *reg)
{
    return fetch_u8(cpu, ram, reg) && register_valid(cpu, *reg);
}

static int fetch_register_pair(CPU *cpu,
                               RAM *ram,
                               uint8_t *first,
                               uint8_t *second)
{
    return fetch_u8(cpu, ram, first) &&
           fetch_u8(cpu, ram, second) &&
           register_valid(cpu, *first) &&
           register_valid(cpu, *second);
}

static int ram_range_valid(const RAM *ram, uint64_t address, size_t width)
{
    if (address <= SIZE_MAX) {
        size_t start = (size_t)address;
        if (start <= ram->size && width <= ram->size - start) {
            return 1;
        }
    }

    return 0;
}

static uint64_t sign_extend(uint64_t value, size_t width)
{
    if (width >= 8) {
        return value;
    }

    unsigned int bits = (unsigned int)(width * 8);
    uint64_t sign = UINT64_C(1) << (bits - 1);

    if ((value & sign) != 0) {
        value |= UINT64_MAX << bits;
    }

    return value;
}

static void set_zero_negative_flags(CPU *cpu, uint64_t result)
{
    /* 산술 명령이 인터럽트 활성화 상태까지 지우지 않도록 보존한다. */
    cpu->flags &= CPU_FLAG_INTERRUPT_ENABLE;

    if (result == 0) {
        cpu->flags |= CPU_FLAG_ZERO;
    }
    if ((result & SIGN_BIT) != 0) {
        cpu->flags |= CPU_FLAG_NEGATIVE;
    }
}

static void set_add_flags(CPU *cpu,
                          uint64_t left,
                          uint64_t right,
                          uint64_t result)
{
    set_zero_negative_flags(cpu, result);

    if (result < left) {
        cpu->flags |= CPU_FLAG_CARRY;
    }
    if (((~(left ^ right) & (left ^ result)) & SIGN_BIT) != 0) {
        cpu->flags |= CPU_FLAG_OVERFLOW;
    }
}

static void set_sub_flags(CPU *cpu,
                          uint64_t left,
                          uint64_t right,
                          uint64_t result)
{
    set_zero_negative_flags(cpu, result);

    /* SUB/CMP의 C는 borrow가 없을 때 1이다. */
    if (left >= right) {
        cpu->flags |= CPU_FLAG_CARRY;
    }
    if ((((left ^ right) & (left ^ result)) & SIGN_BIT) != 0) {
        cpu->flags |= CPU_FLAG_OVERFLOW;
    }
}

static int stack_push_u64(CPU *cpu, RAM *ram, uint64_t value)
{
    uint64_t sp = cpu->registers[REGISTER_SP];
    if (sp < 8) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_STACK_FAULT,
                                    sp,
                                    MMU_ACCESS_WRITE,
                                    MMU_RESULT_OK);
    }

    uint64_t next_sp = sp - 8;
    if (!memory_write(cpu,
                      ram,
                      next_sp,
                      8,
                      MMU_ACCESS_WRITE,
                      0,
                      CPU_EXCEPTION_STACK_FAULT,
                      1,
                      value)) {
        return 0;
    }
    cpu->registers[REGISTER_SP] = next_sp;
    return 1;
}

static int stack_pop_u64(CPU *cpu, RAM *ram, uint64_t *value)
{
    uint64_t sp = cpu->registers[REGISTER_SP];
    if (!memory_read(cpu,
                     ram,
                     sp,
                     8,
                     MMU_ACCESS_READ,
                     0,
                     CPU_EXCEPTION_STACK_FAULT,
                     1,
                     value)) {
        return 0;
    }
    cpu->registers[REGISTER_SP] = sp + 8;
    return 1;
}

static int read_vector(CPU *cpu,
                       RAM *ram,
                       size_t vector_index,
                       uint64_t *handler)
{
    if (!cpu->vbr_enabled || handler == NULL ||
        vector_index >= CPU_VECTOR_ENTRY_COUNT) {
        return 0;
    }

    uint64_t offset = (uint64_t)vector_index * CPU_VECTOR_ENTRY_SIZE;
    if (cpu->vbr > UINT64_MAX - offset) {
        return 0;
    }

    uint64_t entry_address = cpu->vbr + offset;
    return ram_range_valid(ram, entry_address, CPU_VECTOR_ENTRY_SIZE) &&
           ram_read(ram, entry_address, CPU_VECTOR_ENTRY_SIZE, handler) &&
           *handler != UINT64_MAX &&
           instruction_address_valid_for_mode(cpu,
                                              ram,
                                              *handler,
                                              CPU_MODE_SUPERVISOR,
                                              0);
}

static int enter_privileged_handler(CPU *cpu,
                                    RAM *ram,
                                    uint64_t handler,
                                    uint64_t return_address,
                                    int exception_frame,
                                    const char *kind)
{
    CPUMode previous_mode = cpu->mode;
    uint64_t previous_sp = cpu->registers[REGISTER_SP];
    uint64_t stack_top = previous_mode == CPU_MODE_USER
                             ? cpu->ksp
                             : previous_sp;
    size_t frame_size = previous_mode == CPU_MODE_USER ? 24U : 16U;
    uint64_t saved_flags = make_saved_flags(cpu, exception_frame);

    if (stack_top < frame_size) {
        fprintf(stderr,
                "CPUError: cannot enter %s handler: invalid kernel stack\n",
                kind);
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_STACK_FAULT,
                                    stack_top,
                                    MMU_ACCESS_WRITE,
                                    MMU_RESULT_OK);
    }

    uint64_t frame = stack_top - (uint64_t)frame_size;
    cpu->mode = CPU_MODE_SUPERVISOR;
    int accessible =
        ram_write_accessible(cpu, ram, frame, 8, 0) &&
        ram_write_accessible(cpu, ram, frame + 8, 8, 0) &&
        (previous_mode != CPU_MODE_USER ||
         ram_write_accessible(cpu, ram, frame + 16, 8, 0));
    int written = accessible &&
        memory_write(cpu,
                     ram,
                     frame,
                     8,
                     MMU_ACCESS_WRITE,
                     0,
                     CPU_EXCEPTION_STACK_FAULT,
                     0,
                     return_address) &&
        memory_write(cpu,
                     ram,
                     frame + 8,
                     8,
                     MMU_ACCESS_WRITE,
                     0,
                     CPU_EXCEPTION_STACK_FAULT,
                     0,
                     saved_flags) &&
        (previous_mode != CPU_MODE_USER ||
         memory_write(cpu,
                      ram,
                      frame + 16,
                      8,
                      MMU_ACCESS_WRITE,
                      0,
                      CPU_EXCEPTION_STACK_FAULT,
                      0,
                      previous_sp));
    if (!written) {
        cpu->mode = previous_mode;
        cpu->registers[REGISTER_SP] = previous_sp;
        fprintf(stderr,
                "CPUError: cannot write %s frame\n",
                kind);
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_STACK_FAULT,
                                    frame,
                                    MMU_ACCESS_WRITE,
                                    MMU_RESULT_OK);
    }

    cpu->registers[REGISTER_SP] = frame;
    cpu->flags &= ~CPU_FLAG_INTERRUPT_ENABLE;
    cpu->pc = handler;
    cpu->waiting = 0;
    return 1;
}

static int deliver_exception(CPU *cpu, RAM *ram)
{
    CPUException exception = (CPUException)cpu->ecause;
    if (exception <= CPU_EXCEPTION_NONE ||
        exception >= CPU_EXCEPTION_COUNT) {
        exception = CPU_EXCEPTION_ILLEGAL_INSTRUCTION;
        cpu->ecause = (uint64_t)exception;
    }

    size_t vector_index = CPU_VECTOR_EXCEPTION_BASE +
                          (size_t)exception;
    uint64_t handler;
    if (cpu->exception_active ||
        !read_vector(cpu, ram, vector_index, &handler)) {
        fprintf(stderr,
                "CPUError: unhandled exception %u at PC=0x%llX, address=0x%llX\n",
                (unsigned int)exception,
                (unsigned long long)cpu->epc,
                (unsigned long long)cpu->badaddr);
        return 0;
    }

    if (!enter_privileged_handler(cpu,
                                  ram,
                                  handler,
                                  cpu->epc,
                                  1,
                                  "exception")) {
        return 0;
    }

    cpu->exception_pending = 0;
    cpu->exception_active = 1;
    return 1;
}

static int execute_nop(CPU *cpu, RAM *ram)
{
    (void)cpu;
    (void)ram;
    return 1;
}

static int execute_halt(CPU *cpu, RAM *ram)
{
    (void)ram;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    cpu->waiting = 0;
    cpu->halted = 1;
    return 1;
}

static int execute_wait(CPU *cpu, RAM *ram)
{
    (void)ram;
    cpu->waiting = 1;
    return 1;
}

static int execute_movi64(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint64_t immediate;

    if (!fetch_register(cpu, ram, &destination) ||
        !fetch_u64_le(cpu, ram, &immediate)) {
        return 0;
    }

    cpu->registers[destination] = immediate;
    return 1;
}

static int execute_mov(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    cpu->registers[destination] = cpu->registers[source];
    return 1;
}

static int fetch_register_immediate32(CPU *cpu,
                                      RAM *ram,
                                      uint8_t *reg,
                                      uint32_t *immediate)
{
    return fetch_register(cpu, ram, reg) &&
           fetch_u32_le(cpu, ram, immediate);
}

static int execute_movi32u(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &destination,
                                    &immediate)) {
        return 0;
    }
    cpu->registers[destination] = immediate;
    return 1;
}

static int execute_movi32s(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &destination,
                                    &immediate)) {
        return 0;
    }
    cpu->registers[destination] = sign_extend(immediate, 4);
    return 1;
}

static int execute_addi32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &destination,
                                    &immediate)) {
        return 0;
    }
    uint64_t left = cpu->registers[destination];
    uint64_t right = sign_extend(immediate, 4);
    uint64_t result = left + right;
    cpu->registers[destination] = result;
    set_add_flags(cpu, left, right, result);
    return 1;
}

static int execute_cmpi32(CPU *cpu, RAM *ram)
{
    uint8_t left_register;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &left_register,
                                    &immediate)) {
        return 0;
    }
    uint64_t left = cpu->registers[left_register];
    uint64_t right = sign_extend(immediate, 4);
    set_sub_flags(cpu, left, right, left - right);
    return 1;
}

static int execute_andi32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &destination,
                                    &immediate)) {
        return 0;
    }
    cpu->registers[destination] &= immediate;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_ori32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &destination,
                                    &immediate)) {
        return 0;
    }
    cpu->registers[destination] |= immediate;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_xori32(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &destination,
                                    &immediate)) {
        return 0;
    }
    cpu->registers[destination] ^= immediate;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_testi32(CPU *cpu, RAM *ram)
{
    uint8_t left_register;
    uint32_t immediate;
    if (!fetch_register_immediate32(cpu,
                                    ram,
                                    &left_register,
                                    &immediate)) {
        return 0;
    }
    set_zero_negative_flags(cpu,
                            cpu->registers[left_register] & immediate);
    return 1;
}

static int execute_lea(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t base;
    uint32_t displacement;
    if (!fetch_register_pair(cpu, ram, &destination, &base) ||
        !fetch_u32_le(cpu, ram, &displacement)) {
        return 0;
    }
    cpu->registers[destination] =
        cpu->registers[base] + sign_extend(displacement, 4);
    return 1;
}

static int execute_load(CPU *cpu, RAM *ram, size_t width, int is_signed)
{
    uint8_t destination;
    uint8_t address_register;

    if (!fetch_register_pair(cpu, ram, &destination, &address_register)) {
        return 0;
    }

    uint64_t address = cpu->registers[address_register];
    uint64_t value;

    if (!memory_read(cpu,
                     ram,
                     address,
                     width,
                     MMU_ACCESS_READ,
                     1,
                     CPU_EXCEPTION_DATA_ACCESS,
                     1,
                     &value)) {
        return 0;
    }

    cpu->registers[destination] = is_signed ? sign_extend(value, width) : value;
    return 1;
}

static int execute_load8u(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 1, 0);
}

static int execute_load16u(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 2, 0);
}

static int execute_load32u(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 4, 0);
}

static int execute_load64(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 8, 0);
}

static int execute_load8s(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 1, 1);
}

static int execute_load16s(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 2, 1);
}

static int execute_load32s(CPU *cpu, RAM *ram)
{
    return execute_load(cpu, ram, 4, 1);
}

static int execute_store(CPU *cpu, RAM *ram, size_t width)
{
    uint8_t address_register;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &address_register, &source)) {
        return 0;
    }

    uint64_t address = cpu->registers[address_register];
    if (!memory_write(cpu,
                      ram,
                      address,
                      width,
                      MMU_ACCESS_WRITE,
                      1,
                      CPU_EXCEPTION_DATA_ACCESS,
                      1,
                      cpu->registers[source])) {
        return 0;
    }

    return 1;
}

static int execute_store8(CPU *cpu, RAM *ram)
{
    return execute_store(cpu, ram, 1);
}

static int execute_store16(CPU *cpu, RAM *ram)
{
    return execute_store(cpu, ram, 2);
}

static int execute_store32(CPU *cpu, RAM *ram)
{
    return execute_store(cpu, ram, 4);
}

static int execute_store64(CPU *cpu, RAM *ram)
{
    return execute_store(cpu, ram, 8);
}

static int execute_load_offset(CPU *cpu,
                               RAM *ram,
                               size_t width,
                               int is_signed)
{
    uint8_t destination;
    uint8_t base;
    uint32_t displacement;
    if (!fetch_register_pair(cpu, ram, &destination, &base) ||
        !fetch_u32_le(cpu, ram, &displacement)) {
        return 0;
    }

    uint64_t address = cpu->registers[base] +
                       sign_extend(displacement, 4);
    uint64_t value;
    if (!memory_read(cpu,
                     ram,
                     address,
                     width,
                     MMU_ACCESS_READ,
                     1,
                     CPU_EXCEPTION_DATA_ACCESS,
                     1,
                     &value)) {
        return 0;
    }
    cpu->registers[destination] = is_signed
                                      ? sign_extend(value, width)
                                      : value;
    return 1;
}

static int execute_load8uo(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 1, 0);
}

static int execute_load16uo(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 2, 0);
}

static int execute_load32uo(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 4, 0);
}

static int execute_load64o(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 8, 0);
}

static int execute_load8so(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 1, 1);
}

static int execute_load16so(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 2, 1);
}

static int execute_load32so(CPU *cpu, RAM *ram)
{
    return execute_load_offset(cpu, ram, 4, 1);
}

static int execute_store_offset(CPU *cpu,
                                RAM *ram,
                                size_t width)
{
    uint8_t base;
    uint8_t source;
    uint32_t displacement;
    if (!fetch_register_pair(cpu, ram, &base, &source) ||
        !fetch_u32_le(cpu, ram, &displacement)) {
        return 0;
    }
    uint64_t address = cpu->registers[base] +
                       sign_extend(displacement, 4);
    return memory_write(cpu,
                        ram,
                        address,
                        width,
                        MMU_ACCESS_WRITE,
                        1,
                        CPU_EXCEPTION_DATA_ACCESS,
                        1,
                        cpu->registers[source]);
}

static int execute_store8o(CPU *cpu, RAM *ram)
{
    return execute_store_offset(cpu, ram, 1);
}

static int execute_store16o(CPU *cpu, RAM *ram)
{
    return execute_store_offset(cpu, ram, 2);
}

static int execute_store32o(CPU *cpu, RAM *ram)
{
    return execute_store_offset(cpu, ram, 4);
}

static int execute_store64o(CPU *cpu, RAM *ram)
{
    return execute_store_offset(cpu, ram, 8);
}

static int execute_add(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t left = cpu->registers[destination];
    uint64_t right = cpu->registers[source];
    uint64_t result = left + right;

    cpu->registers[destination] = result;
    set_add_flags(cpu, left, right, result);
    return 1;
}

static int execute_sub(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t left = cpu->registers[destination];
    uint64_t right = cpu->registers[source];
    uint64_t result = left - right;

    cpu->registers[destination] = result;
    set_sub_flags(cpu, left, right, result);
    return 1;
}

static int execute_mul(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t left = cpu->registers[destination];
    uint64_t right = cpu->registers[source];
    uint64_t result = left * right;
    int overflow = left != 0 && right > UINT64_MAX / left;

    cpu->registers[destination] = result;
    set_zero_negative_flags(cpu, result);
    if (overflow) {
        cpu->flags |= CPU_FLAG_CARRY;
    }
    return 1;
}

static int execute_divu(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t divisor = cpu->registers[source];
    if (divisor == 0) {
        return set_exception(cpu,
                             CPU_EXCEPTION_DIVIDE_BY_ZERO,
                             0,
                             0);
    }

    cpu->registers[destination] /= divisor;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_modu(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t divisor = cpu->registers[source];
    if (divisor == 0) {
        return set_exception(cpu,
                             CPU_EXCEPTION_DIVIDE_BY_ZERO,
                             0,
                             0);
    }

    cpu->registers[destination] %= divisor;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static uint64_t twos_complement_magnitude(uint64_t value)
{
    return (value & SIGN_BIT) != 0 ? (~value + 1) : value;
}

static uint64_t apply_twos_complement_sign(uint64_t magnitude, int negative)
{
    return negative ? (~magnitude + 1) : magnitude;
}

static int execute_divs(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t dividend = cpu->registers[destination];
    uint64_t divisor = cpu->registers[source];

    if (divisor == 0) {
        return set_exception(cpu,
                             CPU_EXCEPTION_DIVIDE_BY_ZERO,
                             0,
                             0);
    }
    if (dividend == SIGN_BIT && divisor == UINT64_MAX) {
        return set_exception(cpu,
                             CPU_EXCEPTION_ARITHMETIC_OVERFLOW,
                             0,
                             0);
    }

    int negative = ((dividend ^ divisor) & SIGN_BIT) != 0;
    uint64_t result = twos_complement_magnitude(dividend) /
                      twos_complement_magnitude(divisor);

    cpu->registers[destination] =
        apply_twos_complement_sign(result, negative);
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_mods(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    uint64_t dividend = cpu->registers[destination];
    uint64_t divisor = cpu->registers[source];

    if (divisor == 0) {
        return set_exception(cpu,
                             CPU_EXCEPTION_DIVIDE_BY_ZERO,
                             0,
                             0);
    }

    uint64_t result = twos_complement_magnitude(dividend) %
                      twos_complement_magnitude(divisor);

    cpu->registers[destination] =
        apply_twos_complement_sign(result, (dividend & SIGN_BIT) != 0);
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_and(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    cpu->registers[destination] &= cpu->registers[source];
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_or(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    cpu->registers[destination] |= cpu->registers[source];
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_xor(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t source;

    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }

    cpu->registers[destination] ^= cpu->registers[source];
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_not(CPU *cpu, RAM *ram)
{
    uint8_t destination;

    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }

    cpu->registers[destination] = ~cpu->registers[destination];
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int fetch_shift_operands(CPU *cpu,
                                RAM *ram,
                                uint8_t *destination,
                                uint8_t *amount)
{
    if (!fetch_u8(cpu, ram, destination) ||
        !fetch_u8(cpu, ram, amount) ||
        !register_valid(cpu, *destination)) {
        return 0;
    }

    if (*amount >= REGISTER_BITS) {
        return set_exception(cpu,
                             CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                             0,
                             0);
    }

    return 1;
}

static uint64_t arithmetic_shift_right(uint64_t value,
                                       unsigned int amount)
{
    if (amount == 0) {
        return value;
    }
    uint64_t result = value >> amount;
    if ((value & SIGN_BIT) != 0) {
        result |= UINT64_MAX << (REGISTER_BITS - amount);
    }
    return result;
}

static int execute_shl(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t amount;

    if (!fetch_shift_operands(cpu, ram, &destination, &amount)) {
        return 0;
    }

    cpu->registers[destination] <<= amount;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_shr(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t amount;

    if (!fetch_shift_operands(cpu, ram, &destination, &amount)) {
        return 0;
    }

    cpu->registers[destination] >>= amount;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_sar(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t amount;

    if (!fetch_shift_operands(cpu, ram, &destination, &amount)) {
        return 0;
    }

    cpu->registers[destination] = arithmetic_shift_right(
        cpu->registers[destination],
        amount);

    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_shlv(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t amount_register;
    if (!fetch_register_pair(cpu,
                             ram,
                             &destination,
                             &amount_register)) {
        return 0;
    }
    unsigned int amount =
        (unsigned int)(cpu->registers[amount_register] & UINT64_C(63));
    cpu->registers[destination] <<= amount;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_shrv(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t amount_register;
    if (!fetch_register_pair(cpu,
                             ram,
                             &destination,
                             &amount_register)) {
        return 0;
    }
    unsigned int amount =
        (unsigned int)(cpu->registers[amount_register] & UINT64_C(63));
    cpu->registers[destination] >>= amount;
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_sarv(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint8_t amount_register;
    if (!fetch_register_pair(cpu,
                             ram,
                             &destination,
                             &amount_register)) {
        return 0;
    }
    unsigned int amount =
        (unsigned int)(cpu->registers[amount_register] & UINT64_C(63));
    cpu->registers[destination] = arithmetic_shift_right(
        cpu->registers[destination],
        amount);
    set_zero_negative_flags(cpu, cpu->registers[destination]);
    return 1;
}

static int execute_extend(CPU *cpu,
                          RAM *ram,
                          size_t width,
                          int is_signed)
{
    uint8_t destination;
    uint8_t source;
    if (!fetch_register_pair(cpu, ram, &destination, &source)) {
        return 0;
    }
    uint64_t mask = width == 1
                        ? UINT64_C(0xFF)
                        : width == 2
                              ? UINT64_C(0xFFFF)
                              : UINT64_C(0xFFFFFFFF);
    uint64_t value = cpu->registers[source] & mask;
    cpu->registers[destination] = is_signed
                                      ? sign_extend(value, width)
                                      : value;
    return 1;
}

static int execute_sext8(CPU *cpu, RAM *ram)
{
    return execute_extend(cpu, ram, 1, 1);
}

static int execute_sext16(CPU *cpu, RAM *ram)
{
    return execute_extend(cpu, ram, 2, 1);
}

static int execute_sext32(CPU *cpu, RAM *ram)
{
    return execute_extend(cpu, ram, 4, 1);
}

static int execute_zext8(CPU *cpu, RAM *ram)
{
    return execute_extend(cpu, ram, 1, 0);
}

static int execute_zext16(CPU *cpu, RAM *ram)
{
    return execute_extend(cpu, ram, 2, 0);
}

static int execute_zext32(CPU *cpu, RAM *ram)
{
    return execute_extend(cpu, ram, 4, 0);
}

static int execute_cmp(CPU *cpu, RAM *ram)
{
    uint8_t left_register;
    uint8_t right_register;

    if (!fetch_register_pair(cpu, ram, &left_register, &right_register)) {
        return 0;
    }

    uint64_t left = cpu->registers[left_register];
    uint64_t right = cpu->registers[right_register];
    set_sub_flags(cpu, left, right, left - right);
    return 1;
}

static int execute_branch(CPU *cpu, RAM *ram, int condition)
{
    uint64_t target;

    if (!fetch_u64_le(cpu, ram, &target)) {
        return 0;
    }

    if (!condition) {
        return 1;
    }
    if (!instruction_address_valid(cpu, ram, target)) {
        return 0;
    }

    cpu->pc = target;
    return 1;
}

static int execute_jump(CPU *cpu, RAM *ram)
{
    return execute_branch(cpu, ram, 1);
}

static int execute_jumpr(CPU *cpu, RAM *ram)
{
    uint8_t target_register;
    if (!fetch_register(cpu, ram, &target_register)) {
        return 0;
    }
    uint64_t target = cpu->registers[target_register];
    if (!instruction_address_valid(cpu, ram, target)) {
        return 0;
    }
    cpu->pc = target;
    return 1;
}

static int relative_target(CPU *cpu,
                           uint32_t displacement,
                           uint64_t *target)
{
    if (cpu == NULL || target == NULL) {
        return 0;
    }
    *target = cpu->pc + sign_extend(displacement, 4);
    return 1;
}

static int execute_jumprel(CPU *cpu, RAM *ram)
{
    uint32_t displacement;
    uint64_t target;
    if (!fetch_u32_le(cpu, ram, &displacement) ||
        !relative_target(cpu, displacement, &target) ||
        !instruction_address_valid(cpu, ram, target)) {
        return 0;
    }
    cpu->pc = target;
    return 1;
}

static int evaluate_condition(CPU *cpu,
                              uint8_t condition,
                              int *matches)
{
    if (matches == NULL || condition >= CPU_CONDITION_COUNT) {
        return set_exception(cpu,
                             CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                             0,
                             0);
    }

    int zero = (cpu->flags & CPU_FLAG_ZERO) != 0;
    int negative = (cpu->flags & CPU_FLAG_NEGATIVE) != 0;
    int carry = (cpu->flags & CPU_FLAG_CARRY) != 0;
    int overflow = (cpu->flags & CPU_FLAG_OVERFLOW) != 0;
    int unordered = (cpu->flags & CPU_FLAG_UNORDERED) != 0;
    switch ((CPUCondition)condition) {
        case CPU_CONDITION_ALWAYS:
            *matches = 1;
            break;
        case CPU_CONDITION_EQ:
            *matches = zero;
            break;
        case CPU_CONDITION_NE:
            *matches = !zero;
            break;
        case CPU_CONDITION_LT:
            *matches = !unordered && negative != overflow;
            break;
        case CPU_CONDITION_LE:
            *matches = !unordered && (zero || negative != overflow);
            break;
        case CPU_CONDITION_GT:
            *matches = !unordered && !zero && negative == overflow;
            break;
        case CPU_CONDITION_GE:
            *matches = !unordered && negative == overflow;
            break;
        case CPU_CONDITION_LTU:
            *matches = !carry;
            break;
        case CPU_CONDITION_LEU:
            *matches = !carry || zero;
            break;
        case CPU_CONDITION_GTU:
            *matches = carry && !zero;
            break;
        case CPU_CONDITION_GEU:
            *matches = carry;
            break;
        case CPU_CONDITION_ORDERED:
            *matches = !unordered;
            break;
        case CPU_CONDITION_UNORDERED:
            *matches = unordered;
            break;
        case CPU_CONDITION_COUNT:
            return 0;
    }
    return 1;
}

static int execute_brcc(CPU *cpu, RAM *ram)
{
    uint8_t condition;
    uint32_t displacement;
    int matches;
    if (!fetch_u8(cpu, ram, &condition) ||
        !fetch_u32_le(cpu, ram, &displacement) ||
        !evaluate_condition(cpu, condition, &matches)) {
        return 0;
    }
    if (!matches) {
        return 1;
    }
    uint64_t target;
    if (!relative_target(cpu, displacement, &target) ||
        !instruction_address_valid(cpu, ram, target)) {
        return 0;
    }
    cpu->pc = target;
    return 1;
}

static int execute_jz(CPU *cpu, RAM *ram)
{
    return execute_branch(cpu, ram, (cpu->flags & CPU_FLAG_ZERO) != 0);
}

static int execute_jnz(CPU *cpu, RAM *ram)
{
    return execute_branch(cpu, ram, (cpu->flags & CPU_FLAG_ZERO) == 0);
}

static int execute_jlt(CPU *cpu, RAM *ram)
{
    int negative = (cpu->flags & CPU_FLAG_NEGATIVE) != 0;
    int overflow = (cpu->flags & CPU_FLAG_OVERFLOW) != 0;
    int unordered = (cpu->flags & CPU_FLAG_UNORDERED) != 0;
    return execute_branch(cpu, ram, !unordered && negative != overflow);
}

static int execute_jle(CPU *cpu, RAM *ram)
{
    int zero = (cpu->flags & CPU_FLAG_ZERO) != 0;
    int negative = (cpu->flags & CPU_FLAG_NEGATIVE) != 0;
    int overflow = (cpu->flags & CPU_FLAG_OVERFLOW) != 0;
    int unordered = (cpu->flags & CPU_FLAG_UNORDERED) != 0;
    return execute_branch(cpu,
                          ram,
                          !unordered && (zero || negative != overflow));
}

static int execute_jgt(CPU *cpu, RAM *ram)
{
    int zero = (cpu->flags & CPU_FLAG_ZERO) != 0;
    int negative = (cpu->flags & CPU_FLAG_NEGATIVE) != 0;
    int overflow = (cpu->flags & CPU_FLAG_OVERFLOW) != 0;
    int unordered = (cpu->flags & CPU_FLAG_UNORDERED) != 0;
    return execute_branch(cpu,
                          ram,
                          !unordered && !zero && negative == overflow);
}

static int execute_jge(CPU *cpu, RAM *ram)
{
    int negative = (cpu->flags & CPU_FLAG_NEGATIVE) != 0;
    int overflow = (cpu->flags & CPU_FLAG_OVERFLOW) != 0;
    int unordered = (cpu->flags & CPU_FLAG_UNORDERED) != 0;
    return execute_branch(cpu, ram, !unordered && negative == overflow);
}

static int execute_jltu(CPU *cpu, RAM *ram)
{
    return execute_branch(cpu, ram, (cpu->flags & CPU_FLAG_CARRY) == 0);
}

static int execute_jleu(CPU *cpu, RAM *ram)
{
    int zero = (cpu->flags & CPU_FLAG_ZERO) != 0;
    int carry = (cpu->flags & CPU_FLAG_CARRY) != 0;
    return execute_branch(cpu, ram, !carry || zero);
}

static int execute_jgtu(CPU *cpu, RAM *ram)
{
    int zero = (cpu->flags & CPU_FLAG_ZERO) != 0;
    int carry = (cpu->flags & CPU_FLAG_CARRY) != 0;
    return execute_branch(cpu, ram, carry && !zero);
}

static int execute_jgeu(CPU *cpu, RAM *ram)
{
    return execute_branch(cpu, ram, (cpu->flags & CPU_FLAG_CARRY) != 0);
}

static int execute_call(CPU *cpu, RAM *ram)
{
    uint64_t target;

    if (!fetch_u64_le(cpu, ram, &target)) {
        return 0;
    }
    if (!instruction_address_valid(cpu, ram, target)) {
        return 0;
    }

    if (!stack_push_u64(cpu, ram, cpu->pc)) {
        return 0;
    }

    cpu->pc = target;
    return 1;
}

static int execute_callr(CPU *cpu, RAM *ram)
{
    uint8_t target_register;
    if (!fetch_register(cpu, ram, &target_register)) {
        return 0;
    }
    uint64_t target = cpu->registers[target_register];
    if (!instruction_address_valid(cpu, ram, target) ||
        !stack_push_u64(cpu, ram, cpu->pc)) {
        return 0;
    }
    cpu->pc = target;
    return 1;
}

static int execute_callrel(CPU *cpu, RAM *ram)
{
    uint32_t displacement;
    uint64_t target;
    if (!fetch_u32_le(cpu, ram, &displacement) ||
        !relative_target(cpu, displacement, &target) ||
        !instruction_address_valid(cpu, ram, target) ||
        !stack_push_u64(cpu, ram, cpu->pc)) {
        return 0;
    }
    cpu->pc = target;
    return 1;
}

static int execute_ret(CPU *cpu, RAM *ram)
{
    uint64_t sp = cpu->registers[REGISTER_SP];
    uint64_t return_address;

    if (!memory_read(cpu,
                     ram,
                     sp,
                     8,
                     MMU_ACCESS_READ,
                     0,
                     CPU_EXCEPTION_STACK_FAULT,
                     1,
                     &return_address)) {
        return 0;
    }
    if (!instruction_address_valid(cpu, ram, return_address)) {
        return 0;
    }

    cpu->registers[REGISTER_SP] = sp + 8;
    cpu->pc = return_address;
    return 1;
}

static int execute_push(CPU *cpu, RAM *ram)
{
    uint8_t source;

    if (!fetch_register(cpu, ram, &source)) {
        return 0;
    }

    return stack_push_u64(cpu, ram, cpu->registers[source]);
}

static int execute_pop(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    uint64_t value;

    if (!fetch_register(cpu, ram, &destination) ||
        !stack_pop_u64(cpu, ram, &value)) {
        return 0;
    }

    cpu->registers[destination] = value;
    return 1;
}

static int execute_ei(CPU *cpu, RAM *ram)
{
    (void)ram;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    cpu->flags |= CPU_FLAG_INTERRUPT_ENABLE;
    return 1;
}

static int execute_di(CPU *cpu, RAM *ram)
{
    (void)ram;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    cpu->flags &= ~CPU_FLAG_INTERRUPT_ENABLE;
    return 1;
}

static int execute_iret(CPU *cpu, RAM *ram)
{
    if (!require_supervisor(cpu)) {
        return 0;
    }

    uint64_t sp = cpu->registers[REGISTER_SP];
    uint64_t return_address;
    uint64_t saved_flags;
    if (sp > UINT64_MAX - 8 ||
        !memory_read(cpu,
                     ram,
                     sp,
                     8,
                     MMU_ACCESS_READ,
                     0,
                     CPU_EXCEPTION_STACK_FAULT,
                     1,
                     &return_address) ||
        !memory_read(cpu,
                     ram,
                     sp + 8,
                     8,
                     MMU_ACCESS_READ,
                     0,
                     CPU_EXCEPTION_STACK_FAULT,
                     1,
                     &saved_flags)) {
        return 0;
    }

    CPUMode return_mode =
        (saved_flags & CPU_SAVED_FLAG_USER_MODE) != 0
            ? CPU_MODE_USER
            : CPU_MODE_SUPERVISOR;
    uint64_t saved_user_sp = 0;
    if (return_mode == CPU_MODE_USER &&
        (sp > UINT64_MAX - 24 ||
         !memory_read(cpu,
                      ram,
                      sp + 16,
                      8,
                      MMU_ACCESS_READ,
                      0,
                      CPU_EXCEPTION_STACK_FAULT,
                      1,
                      &saved_user_sp))) {
        return 0;
    }
    if (return_mode == CPU_MODE_SUPERVISOR &&
        sp > UINT64_MAX - 16) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_STACK_FAULT,
                                    sp,
                                    MMU_ACCESS_READ,
                                    MMU_RESULT_OK);
    }
    if (!instruction_address_valid_for_mode(cpu,
                                            ram,
                                            return_address,
                                            return_mode,
                                            1)) {
        return 0;
    }

    if (return_mode == CPU_MODE_USER) {
        cpu->ksp = sp + 24;
        cpu->registers[REGISTER_SP] = saved_user_sp;
    } else {
        cpu->registers[REGISTER_SP] = sp + 16;
    }
    cpu->pc = return_address;
    cpu->flags = saved_flags & CPU_FLAG_MASK;
    cpu->mode = return_mode;
    if ((saved_flags & CPU_SAVED_FLAG_EXCEPTION_FRAME) != 0) {
        cpu->exception_active = 0;
    }
    return 1;
}

static int execute_cas64(CPU *cpu, RAM *ram)
{
    uint8_t address_register;
    uint8_t expected_register;
    uint8_t desired_register;
    uint8_t result_register;

    if (!fetch_u8(cpu, ram, &address_register) ||
        !fetch_u8(cpu, ram, &expected_register) ||
        !fetch_u8(cpu, ram, &desired_register) ||
        !fetch_u8(cpu, ram, &result_register) ||
        !register_valid(cpu, address_register) ||
        !register_valid(cpu, expected_register) ||
        !register_valid(cpu, desired_register) ||
        !register_valid(cpu, result_register)) {
        return 0;
    }

    uint64_t expected = cpu->registers[expected_register];
    uint64_t observed = expected;
    uint64_t virtual_address = cpu->registers[address_register];
    uint64_t physical_address;
    if (!translate_address(cpu,
                           ram,
                           virtual_address,
                           MMU_ACCESS_ATOMIC,
                           1,
                           &physical_address)) {
        return 0;
    }
    if (cpu->bus == NULL ||
        !bus_compare_exchange64(cpu->bus,
                                physical_address,
                                &observed,
                                cpu->registers[desired_register])) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_DATA_ACCESS,
                                    virtual_address,
                                    MMU_ACCESS_ATOMIC,
                                    MMU_RESULT_OK);
    }

    cpu->registers[result_register] = observed;
    cpu->flags &= CPU_FLAG_INTERRUPT_ENABLE;
    if (observed == expected) {
        cpu->flags |= CPU_FLAG_ZERO;
    }
    return 1;
}

static int execute_xchg64(CPU *cpu, RAM *ram)
{
    uint8_t address_register;
    uint8_t value_register;

    if (!fetch_register_pair(cpu,
                             ram,
                             &address_register,
                             &value_register)) {
        return 0;
    }

    uint64_t virtual_address = cpu->registers[address_register];
    uint64_t physical_address;
    if (!translate_address(cpu,
                           ram,
                           virtual_address,
                           MMU_ACCESS_ATOMIC,
                           1,
                           &physical_address)) {
        return 0;
    }

    uint64_t value = cpu->registers[value_register];
    if (cpu->bus == NULL ||
        !bus_exchange64(cpu->bus,
                        physical_address,
                        &value)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_DATA_ACCESS,
                                    virtual_address,
                                    MMU_ACCESS_ATOMIC,
                                    MMU_RESULT_OK);
    }

    cpu->registers[value_register] = value;
    return 1;
}

static int execute_atomic_add64(CPU *cpu, RAM *ram)
{
    uint8_t address_register;
    uint8_t value_register;
    uint8_t result_register;

    if (!fetch_u8(cpu, ram, &address_register) ||
        !fetch_u8(cpu, ram, &value_register) ||
        !fetch_u8(cpu, ram, &result_register) ||
        !register_valid(cpu, address_register) ||
        !register_valid(cpu, value_register) ||
        !register_valid(cpu, result_register)) {
        return 0;
    }

    uint64_t virtual_address = cpu->registers[address_register];
    uint64_t physical_address;
    if (!translate_address(cpu,
                           ram,
                           virtual_address,
                           MMU_ACCESS_ATOMIC,
                           1,
                           &physical_address)) {
        return 0;
    }

    uint64_t value = cpu->registers[value_register];
    if (cpu->bus == NULL ||
        !bus_fetch_add64(cpu->bus,
                         physical_address,
                         &value)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_DATA_ACCESS,
                                    virtual_address,
                                    MMU_ACCESS_ATOMIC,
                                    MMU_RESULT_OK);
    }

    cpu->registers[result_register] = value;
    return 1;
}

static int execute_fence(CPU *cpu, RAM *ram)
{
    (void)cpu;
    (void)ram;
    bus_memory_fence();
    return 1;
}

static int execute_coreid(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->core_id;
    return 1;
}

static int execute_threadid(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->thread_id;
    return 1;
}

static int execute_excause(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->ecause;
    return 1;
}

static int execute_exaddr(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->badaddr;
    return 1;
}

static int execute_expc(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->epc;
    return 1;
}

static int execute_exinfo(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->einfo;
    return 1;
}

static int execute_setvbr(CPU *cpu, RAM *ram)
{
    uint8_t source;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!fetch_register(cpu, ram, &source)) {
        return 0;
    }

    uint64_t address = cpu->registers[source];
    if (!cpu_set_vector_base(cpu, ram, address)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_DATA_ACCESS,
                                    address,
                                    MMU_ACCESS_READ,
                                    MMU_RESULT_OK);
    }
    return 1;
}

static int execute_getvbr(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->vbr;
    return 1;
}

static int execute_getmode(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }

    cpu->registers[destination] = (uint64_t)cpu->mode;
    return 1;
}

static int execute_enteruser(CPU *cpu, RAM *ram)
{
    (void)ram;
    if (!require_supervisor(cpu)) {
        return 0;
    }

    cpu->mode = CPU_MODE_USER;
    return 1;
}

static int execute_syscall(CPU *cpu, RAM *ram)
{
    uint64_t handler;
    if (!read_vector(cpu, ram, CPU_VECTOR_SYSCALL, &handler)) {
        return set_exception(cpu,
                             CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                             0,
                             0);
    }
    return enter_privileged_handler(cpu,
                                    ram,
                                    handler,
                                    cpu->pc,
                                    0,
                                    "system call");
}

static int execute_setksp(CPU *cpu, RAM *ram)
{
    uint8_t source;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!fetch_register(cpu, ram, &source)) {
        return 0;
    }
    cpu->ksp = cpu->registers[source];
    return 1;
}

static int execute_getksp(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }
    cpu->registers[destination] = cpu->ksp;
    return 1;
}

static int page_table_can_execute(RAM *ram,
                                  uint64_t ptbr,
                                  CPUMode mode,
                                  uint64_t address)
{
    uint64_t physical_address;
    return mmu_translate(ram,
                         ptbr,
                         mode == CPU_MODE_USER,
                         address,
                         MMU_ACCESS_EXECUTE,
                         &physical_address) == MMU_RESULT_OK &&
           ram_range_valid(ram, physical_address, 1);
}

static int execute_setptbr(CPU *cpu, RAM *ram)
{
    uint8_t source;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!fetch_register(cpu, ram, &source)) {
        return 0;
    }

    uint64_t address = cpu->registers[source];
    if (!mmu_root_valid(ram, address)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_MMU_CONFIGURATION,
                                    address,
                                    MMU_ACCESS_READ,
                                    MMU_RESULT_INVALID_ROOT);
    }
    if (cpu->mmu_enabled &&
        !page_table_can_execute(ram,
                                address,
                                cpu->mode,
                                cpu->pc)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_MMU_CONFIGURATION,
                                    cpu->pc,
                                    MMU_ACCESS_EXECUTE,
                                    MMU_RESULT_OK);
    }

    cpu->ptbr = address;
    cpu->ptbr_set = 1;
    return 1;
}

static int execute_getptbr(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }

    cpu->registers[destination] = cpu->ptbr;
    return 1;
}

static int execute_mmuon(CPU *cpu, RAM *ram)
{
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (cpu->mmu_enabled) {
        return 1;
    }
    if (!cpu->ptbr_set || !mmu_root_valid(ram, cpu->ptbr)) {
        uint64_t info = cpu->ptbr_set
                            ? CPU_EINFO_BADADDR_VALID |
                                  CPU_EINFO_ACCESS_READ |
                                  CPU_EINFO_PAGE_INVALID_ROOT
                            : CPU_EINFO_PAGE_INVALID_ROOT;
        return set_exception(cpu,
                             CPU_EXCEPTION_MMU_CONFIGURATION,
                             cpu->ptbr,
                             info);
    }
    if (!page_table_can_execute(ram,
                                cpu->ptbr,
                                cpu->mode,
                                cpu->pc)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_MMU_CONFIGURATION,
                                    cpu->pc,
                                    MMU_ACCESS_EXECUTE,
                                    MMU_RESULT_OK);
    }

    cpu->mmu_enabled = 1;
    return 1;
}

static int execute_mmuoff(CPU *cpu, RAM *ram)
{
    if (!require_supervisor(cpu)) {
        return 0;
    }
    if (!cpu->mmu_enabled) {
        return 1;
    }
    if (!ram_range_valid(ram, cpu->pc, 1)) {
        return set_access_exception(cpu,
                                    CPU_EXCEPTION_MMU_CONFIGURATION,
                                    cpu->pc,
                                    MMU_ACCESS_EXECUTE,
                                    MMU_RESULT_OK);
    }

    cpu->mmu_enabled = 0;
    return 1;
}

static int execute_getmmu(CPU *cpu, RAM *ram)
{
    uint8_t destination;
    if (!fetch_register(cpu, ram, &destination)) {
        return 0;
    }

    cpu->registers[destination] = cpu->mmu_enabled ? 1 : 0;
    return 1;
}

static const OpcodeHandler OPCODE_HANDLERS[256] = {
    [OP_HALT]    = execute_halt,
    [OP_MOVI64]  = execute_movi64,
    [OP_ADD]     = execute_add,
    [OP_LOAD8U]  = execute_load8u,
    [OP_STORE8]  = execute_store8,
    [OP_JUMP]    = execute_jump,
    [OP_CALL]    = execute_call,
    [OP_RET]     = execute_ret,
    [OP_MOV]     = execute_mov,
    [OP_LOAD16U] = execute_load16u,
    [OP_LOAD32U] = execute_load32u,
    [OP_LOAD64]  = execute_load64,
    [OP_LOAD8S]  = execute_load8s,
    [OP_LOAD16S] = execute_load16s,
    [OP_LOAD32S] = execute_load32s,
    [OP_STORE16] = execute_store16,
    [OP_STORE32] = execute_store32,
    [OP_STORE64] = execute_store64,
    [OP_SUB]     = execute_sub,
    [OP_MUL]     = execute_mul,
    [OP_DIVU]    = execute_divu,
    [OP_DIVS]    = execute_divs,
    [OP_MODU]    = execute_modu,
    [OP_MODS]    = execute_mods,
    [OP_AND]     = execute_and,
    [OP_OR]      = execute_or,
    [OP_XOR]     = execute_xor,
    [OP_NOT]     = execute_not,
    [OP_SHL]     = execute_shl,
    [OP_SHR]     = execute_shr,
    [OP_SAR]     = execute_sar,
    [OP_CMP]     = execute_cmp,
    [OP_JZ]      = execute_jz,
    [OP_JNZ]     = execute_jnz,
    [OP_JLT]     = execute_jlt,
    [OP_JLE]     = execute_jle,
    [OP_JGT]     = execute_jgt,
    [OP_JGE]     = execute_jge,
    [OP_JLTU]    = execute_jltu,
    [OP_JLEU]    = execute_jleu,
    [OP_JGTU]    = execute_jgtu,
    [OP_JGEU]    = execute_jgeu,
    [OP_PUSH]    = execute_push,
    [OP_POP]     = execute_pop,
    [OP_NOP]     = execute_nop,
    [OP_IRET]    = execute_iret,
    [OP_EI]      = execute_ei,
    [OP_DI]      = execute_di,
    [OP_CAS64]        = execute_cas64,
    [OP_XCHG64]       = execute_xchg64,
    [OP_ATOMIC_ADD64] = execute_atomic_add64,
    [OP_FENCE]        = execute_fence,
    [OP_COREID]       = execute_coreid,
    [OP_THREADID]     = execute_threadid,
    [OP_WAIT]         = execute_wait,
    [OP_EXCAUSE]      = execute_excause,
    [OP_EXADDR]       = execute_exaddr,
    [OP_SETVBR]       = execute_setvbr,
    [OP_GETVBR]       = execute_getvbr,
    [OP_GETMODE]      = execute_getmode,
    [OP_ENTERUSER]    = execute_enteruser,
    [OP_SETPTBR]      = execute_setptbr,
    [OP_GETPTBR]      = execute_getptbr,
    [OP_MMUON]        = execute_mmuon,
    [OP_MMUOFF]       = execute_mmuoff,
    [OP_GETMMU]       = execute_getmmu,
    [OP_EXPC]         = execute_expc,
    [OP_EXINFO]       = execute_exinfo,
    [OP_JUMPR]        = execute_jumpr,
    [OP_CALLR]        = execute_callr,
    [OP_SHLV]         = execute_shlv,
    [OP_SHRV]         = execute_shrv,
    [OP_SARV]         = execute_sarv,
    [OP_MOVI32U]      = execute_movi32u,
    [OP_MOVI32S]      = execute_movi32s,
    [OP_ADDI32]       = execute_addi32,
    [OP_CMPI32]       = execute_cmpi32,
    [OP_ANDI32]       = execute_andi32,
    [OP_ORI32]        = execute_ori32,
    [OP_XORI32]       = execute_xori32,
    [OP_TESTI32]      = execute_testi32,
    [OP_LEA]          = execute_lea,
    [OP_LOAD8UO]      = execute_load8uo,
    [OP_LOAD16UO]     = execute_load16uo,
    [OP_LOAD32UO]     = execute_load32uo,
    [OP_LOAD64O]      = execute_load64o,
    [OP_LOAD8SO]      = execute_load8so,
    [OP_LOAD16SO]     = execute_load16so,
    [OP_LOAD32SO]     = execute_load32so,
    [OP_STORE8O]      = execute_store8o,
    [OP_STORE16O]     = execute_store16o,
    [OP_STORE32O]     = execute_store32o,
    [OP_STORE64O]     = execute_store64o,
    [OP_JUMPREL]      = execute_jumprel,
    [OP_BRCC]         = execute_brcc,
    [OP_CALLREL]      = execute_callrel,
    [OP_SEXT8]        = execute_sext8,
    [OP_SEXT16]       = execute_sext16,
    [OP_SEXT32]       = execute_sext32,
    [OP_ZEXT8]        = execute_zext8,
    [OP_ZEXT16]       = execute_zext16,
    [OP_ZEXT32]       = execute_zext32,
    [OP_SYSCALL]      = execute_syscall,
    [OP_SETKSP]       = execute_setksp,
    [OP_GETKSP]       = execute_getksp
};

int cpu_init(CPU *cpu, const RAM *ram)
{
    if (cpu == NULL || ram == NULL || ram->data == NULL) {
        return 0;
    }

    *cpu = (CPU){0};
    cpu->mode = CPU_MODE_SUPERVISOR;
    return 1;
}

int cpu_set_vector_base(CPU *cpu,
                        const RAM *ram,
                        uint64_t address)
{
    if (cpu == NULL || ram == NULL || (address & UINT64_C(7)) != 0 ||
        !ram_range_valid(ram, address, CPU_VECTOR_TABLE_SIZE)) {
        return 0;
    }

    cpu->vbr = address;
    cpu->vbr_enabled = 1;
    return 1;
}

int cpu_step(CPU *cpu, Bus *bus)
{
    if (cpu == NULL || bus == NULL || bus->ram == NULL ||
        bus->ram->data == NULL) {
        fputs("CPUError: invalid CPU or bus state\n", stderr);
        return 0;
    }

    if (cpu->halted || cpu->waiting) {
        return 1;
    }

    RAM *ram = bus->ram;
    cpu->bus = bus;

    uint64_t instruction_address = cpu->pc;
    cpu->instruction_pc = instruction_address;
    cpu->exception_pending = 0;
    uint8_t opcode;

    if (!fetch_u8(cpu, ram, &opcode)) {
        return deliver_exception(cpu, ram);
    }

    OpcodeHandler handler = OPCODE_HANDLERS[opcode];
    int executed;
    if (handler != NULL) {
        executed = handler(cpu, ram);
    } else if (cpu_vector_opcode(opcode)) {
        executed = cpu_vector_execute(cpu, ram, opcode);
    } else {
        (void)set_exception(cpu,
                            CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                            0,
                            0);
        return deliver_exception(cpu, ram);
    }

    if (executed) {
        return 1;
    }

    if (!cpu->exception_pending) {
        (void)set_exception(cpu,
                            CPU_EXCEPTION_ILLEGAL_INSTRUCTION,
                            0,
                            0);
    }
    return deliver_exception(cpu, ram);
}

int cpu_check_interrupt(CPU *cpu,
                        Bus *bus,
                        InterruptController *interrupts)
{
    if (cpu == NULL || bus == NULL || bus->ram == NULL) {
        return 0;
    }

    RAM *ram = bus->ram;
    cpu->bus = bus;
    if (interrupts == NULL ||
        (cpu->flags & CPU_FLAG_INTERRUPT_ENABLE) == 0) {
        return 1;
    }

    unsigned int line;
    uint64_t handler_address;

    if (!interrupt_controller_take_next(interrupts, &line)) {
        return 1;
    }

    if (!read_vector(cpu,
                     ram,
                     CPU_VECTOR_INTERRUPT_BASE + (size_t)line,
                     &handler_address)) {
        fprintf(stderr,
                "CPUError: missing or invalid vector for interrupt %u\n",
                line);
        return 0;
    }

    return enter_privileged_handler(cpu,
                                    ram,
                                    handler_address,
                                    cpu->pc,
                                    0,
                                    "interrupt");
}

int cpu_run(CPU *cpu,
            Bus *bus,
            InterruptController *interrupts,
            ClockSource *clock)
{
    if (cpu == NULL || bus == NULL || bus->ram == NULL ||
        bus->ram->data == NULL) {
        fputs("CPUError: invalid CPU or bus state\n", stderr);
        return 0;
    }

    cpu->bus = bus;

    InterruptRouter router;
    InterruptRouter *router_pointer = NULL;
    InterruptController *targets[1] = { interrupts };
    if (interrupts != NULL) {
        if (!interrupt_router_init(&router, targets, 1)) {
            return 0;
        }
        router_pointer = &router;
    }

    while (!cpu->halted) {
        if (!cpu_step(cpu, bus)) {
            return 0;
        }

        /* 명령어 하나가 끝날 때 tick을 장치에 전달한 뒤 IRQ를 샘플링한다. */
        uint64_t elapsed_ticks = clock_source_poll(clock);
        bus_tick(bus, elapsed_ticks, router_pointer);

        if (!cpu->halted &&
            !cpu_check_interrupt(cpu, bus, interrupts)) {
            return 0;
        }
    }

    return 1;
}
