#ifndef CPU_H
#define CPU_H

#include <stdint.h>

#include "bus.h"
#include "interrupt.h"
#include "isa.h"
#include "ram.h"
#include "vm_clock.h"

#define CPU_EXCEPTION_COUNT 12U
#define CPU_VECTOR_INTERRUPT_BASE 0U
#define CPU_VECTOR_EXCEPTION_BASE INTERRUPT_LINE_COUNT
#define CPU_VECTOR_SYSCALL \
    (CPU_VECTOR_EXCEPTION_BASE + CPU_EXCEPTION_COUNT)
#define CPU_VECTOR_ENTRY_SIZE 8U
#define CPU_VECTOR_ENTRY_COUNT \
    (CPU_VECTOR_SYSCALL + 1U)
#define CPU_VECTOR_TABLE_SIZE \
    (CPU_VECTOR_ENTRY_COUNT * CPU_VECTOR_ENTRY_SIZE)

typedef enum {
    CPU_EXCEPTION_NONE = 0,
    CPU_EXCEPTION_ILLEGAL_INSTRUCTION = 1,
    CPU_EXCEPTION_INSTRUCTION_ACCESS = 2,
    CPU_EXCEPTION_DATA_ACCESS = 3,
    CPU_EXCEPTION_DIVIDE_BY_ZERO = 4,
    CPU_EXCEPTION_ARITHMETIC_OVERFLOW = 5,
    CPU_EXCEPTION_STACK_FAULT = 6,
    CPU_EXCEPTION_PRIVILEGE_VIOLATION = 7,
    CPU_EXCEPTION_INSTRUCTION_PAGE_FAULT = 8,
    CPU_EXCEPTION_LOAD_PAGE_FAULT = 9,
    CPU_EXCEPTION_STORE_PAGE_FAULT = 10,
    CPU_EXCEPTION_MMU_CONFIGURATION = 11
} CPUException;

typedef enum {
    CPU_MODE_USER = 0,
    CPU_MODE_SUPERVISOR = 1
} CPUMode;

typedef struct {
    uint8_t bytes[VECTOR_REGISTER_SIZE];
} CPUVectorRegister;

typedef enum {
    CPU_FP_INVALID = UINT64_C(1) << 0,
    CPU_FP_DIVIDE_BY_ZERO = UINT64_C(1) << 1,
    CPU_FP_OVERFLOW = UINT64_C(1) << 2,
    CPU_FP_UNDERFLOW = UINT64_C(1) << 3
} CPUFloatingPointStatus;

typedef enum {
    CPU_EINFO_BADADDR_VALID       = UINT64_C(1) << 0,
    CPU_EINFO_ACCESS_READ         = UINT64_C(1) << 1,
    CPU_EINFO_ACCESS_WRITE        = UINT64_C(1) << 2,
    CPU_EINFO_ACCESS_EXECUTE      = UINT64_C(1) << 3,
    CPU_EINFO_ACCESS_ATOMIC       = UINT64_C(1) << 4,
    CPU_EINFO_ORIGIN_USER         = UINT64_C(1) << 5,
    CPU_EINFO_MMU_ENABLED         = UINT64_C(1) << 6,
    CPU_EINFO_PAGE_INVALID_VA     = UINT64_C(1) << 8,
    CPU_EINFO_PAGE_INVALID_ROOT   = UINT64_C(1) << 9,
    CPU_EINFO_PAGE_NOT_PRESENT    = UINT64_C(1) << 10,
    CPU_EINFO_PAGE_MALFORMED      = UINT64_C(1) << 11,
    CPU_EINFO_PAGE_PERMISSION     = UINT64_C(1) << 12
} CPUExceptionInfo;

typedef struct {
    uint64_t registers[REGISTER_COUNT];
    CPUVectorRegister vector_registers[VECTOR_REGISTER_COUNT];
    uint64_t pc;
    uint64_t flags;
    uint64_t fp_status;
    uint64_t vbr;
    uint64_t ptbr;
    uint64_t ksp;
    uint64_t epc;
    uint64_t ecause;
    uint64_t badaddr;
    uint64_t einfo;
    uint64_t instruction_pc;
    uint32_t core_id;
    uint32_t thread_id;
    CPUMode mode;
    int halted;
    int waiting;
    int vbr_enabled;
    int ptbr_set;
    int mmu_enabled;
    int exception_pending;
    int exception_active;
    Bus *bus;
} CPU;

typedef enum {
    CPU_FLAG_ZERO     = UINT64_C(1) << 0,
    CPU_FLAG_NEGATIVE = UINT64_C(1) << 1,
    CPU_FLAG_CARRY    = UINT64_C(1) << 2,
    CPU_FLAG_OVERFLOW = UINT64_C(1) << 3,
    CPU_FLAG_INTERRUPT_ENABLE = UINT64_C(1) << 4,
    CPU_FLAG_UNORDERED = UINT64_C(1) << 5
} CPUFlag;

int cpu_init(CPU *cpu, const RAM *ram);
int cpu_set_vector_base(CPU *cpu,
                        const RAM *ram,
                        uint64_t address);
int cpu_step(CPU *cpu, Bus *bus);
int cpu_check_interrupt(CPU *cpu,
                        Bus *bus,
                        InterruptController *interrupts);
int cpu_run(CPU *cpu,
            Bus *bus,
            InterruptController *interrupts,
            ClockSource *clock);

#endif
