#include "kernel_internal.h"

#include <cvm/intrin.h>

static volatile uint64_t exception_recovery_count;
static uintptr_t demand_test_physical;
static volatile uint64_t null_test_active;
static volatile uint64_t null_fault_count;
static volatile uint64_t store_test_active;
static volatile uint64_t store_fault_count;
static volatile uint64_t execute_test_active;
static volatile uint64_t execute_fault_count;
static uintptr_t kernel_vbr;

static int has_all(uint64_t value, uint64_t required)
{
    return (value & required) == required;
}

int kernel_exception_init(void)
{
    kernel_vbr = kernel_pmm_alloc_page();
    if (kernel_vbr == 0) return 1;

    uint64_t *vectors = kernel_phys_to_virt(kernel_vbr);
    if (vectors == NULL) return 1;
    for (size_t i = 0; i < KERNEL_VECTOR_ENTRY_COUNT; ++i) {
        vectors[i] = UINT64_MAX;
    }
    for (size_t i = 1; i <= 11; ++i) {
        vectors[KERNEL_VECTOR_EXCEPTION_BASE + i] =
            (uint64_t)(uintptr_t)kernel_exception_panic_entry;
    }
    vectors[KERNEL_VECTOR_EXCEPTION_BASE +
            KERNEL_EXCEPTION_INSTRUCTION_PAGE_FAULT] =
        (uint64_t)(uintptr_t)kernel_page_fault_entry;
    vectors[KERNEL_VECTOR_EXCEPTION_BASE +
            KERNEL_EXCEPTION_LOAD_PAGE_FAULT] =
        (uint64_t)(uintptr_t)kernel_page_fault_entry;
    vectors[KERNEL_VECTOR_EXCEPTION_BASE +
            KERNEL_EXCEPTION_STORE_PAGE_FAULT] =
        (uint64_t)(uintptr_t)kernel_page_fault_entry;
    vectors[KERNEL_VECTOR_SYSCALL] =
        (uint64_t)(uintptr_t)kernel_syscall_entry;
    vectors[KERNEL_TIMER_INTERRUPT_LINE] =
        (uint64_t)(uintptr_t)kernel_timer_entry;

    cvm_set_vbr((uint64_t)kernel_vbr);
    if (cvm_get_vbr() != (uint64_t)kernel_vbr) return 1;
    cvm_set_ksp(KERNEL_EXCEPTION_STACK_TOP);
    return 0;
}

int kernel_memory_protection_self_test(void)
{
    null_fault_count = 0;
    null_test_active = 1;
    if (kernel_probe_null_load() != 0 || null_fault_count != 1 ||
        null_test_active != 0) {
        return 1;
    }

    store_fault_count = 0;
    store_test_active = 1;
    if (kernel_probe_rodata_store() != 0 || store_fault_count != 1 ||
        store_test_active != 0) {
        return 1;
    }

    execute_fault_count = 0;
    execute_test_active = 1;
    if (kernel_probe_data_execute() != 0 || execute_fault_count != 1 ||
        execute_test_active != 0) {
        return 1;
    }
    return 0;
}

int kernel_demand_page_self_test(void)
{
    exception_recovery_count = 0;
    volatile uint64_t *address =
        (volatile uint64_t *)(uintptr_t)KERNEL_DEMAND_TEST_ADDRESS;
    uint64_t value = *address;
    return value == 0 && exception_recovery_count == 1 ? 0 : 1;
}

static int handle_load_page_fault(uint64_t address, uint64_t info,
                                  uint64_t *return_pc)
{
    const uint64_t required = KERNEL_EINFO_BADADDR_VALID |
                              KERNEL_EINFO_ACCESS_READ |
                              KERNEL_EINFO_MMU_ENABLED |
                              KERNEL_EINFO_NOT_PRESENT;
    if (!has_all(info, required)) return 1;

    if (address == 0) {
        if (null_test_active != 1 ||
            *return_pc != (uint64_t)(uintptr_t)
                          kernel_null_load_probe_instruction) {
            return 1;
        }
        null_test_active = 0;
        ++null_fault_count;
        *return_pc = (uint64_t)(uintptr_t)kernel_null_load_probe_resume;
        kernel_uart_puts("KERNEL: NULL PAGE BLOCKED\n");
        return 0;
    }

    if (address != KERNEL_DEMAND_TEST_ADDRESS) return 1;
    uintptr_t page = kernel_pmm_alloc_page();
    if (page == 0) return 1;
    demand_test_physical = page;
    if (kernel_map_page(kernel_page_table_root(), (uintptr_t)address, page,
                        KERNEL_PTE_VALID | KERNEL_PTE_READ |
                        KERNEL_PTE_WRITE) != 0) {
        return 1;
    }
    ++exception_recovery_count;
    kernel_uart_puts("KERNEL: PAGE FAULT RECOVERED\n");
    return 0;
}

static int handle_store_page_fault(uint64_t address, uint64_t info,
                                   uint64_t *return_pc)
{
    const uint64_t required = KERNEL_EINFO_BADADDR_VALID |
                              KERNEL_EINFO_ACCESS_WRITE |
                              KERNEL_EINFO_MMU_ENABLED |
                              KERNEL_EINFO_PERMISSION;
    if (!has_all(info, required) ||
        address != (uint64_t)(uintptr_t)&kernel_write_protect_probe ||
        store_test_active != 1 ||
        *return_pc != (uint64_t)(uintptr_t)kernel_store_probe_instruction) {
        return 1;
    }
    store_test_active = 0;
    ++store_fault_count;
    *return_pc = (uint64_t)(uintptr_t)kernel_store_probe_resume;
    kernel_uart_puts("KERNEL: WRITE PROTECT OK\n");
    return 0;
}

static int handle_instruction_page_fault(uint64_t address, uint64_t info,
                                         uint64_t *return_pc)
{
    const uint64_t required = KERNEL_EINFO_BADADDR_VALID |
                              KERNEL_EINFO_ACCESS_EXECUTE |
                              KERNEL_EINFO_MMU_ENABLED |
                              KERNEL_EINFO_PERMISSION;
    if (!has_all(info, required) ||
        address != (uint64_t)(uintptr_t)&kernel_execution_protect_probe ||
        execute_test_active != 1 ||
        *return_pc != (uint64_t)(uintptr_t)kernel_execute_probe_instruction) {
        return 1;
    }
    execute_test_active = 0;
    ++execute_fault_count;
    *return_pc = (uint64_t)(uintptr_t)kernel_execute_probe_resume;
    kernel_uart_puts("KERNEL: NX PROTECT OK\n");
    return 0;
}

int kernel_handle_page_fault(uint64_t *return_pc)
{
    uint64_t cause = cvm_exception_cause();
    uint64_t address = cvm_exception_address();
    uint64_t info = cvm_exception_info();
    if (cause == KERNEL_EXCEPTION_LOAD_PAGE_FAULT) {
        return handle_load_page_fault(address, info, return_pc);
    }
    if (cause == KERNEL_EXCEPTION_STORE_PAGE_FAULT) {
        return handle_store_page_fault(address, info, return_pc);
    }
    if (cause == KERNEL_EXCEPTION_INSTRUCTION_PAGE_FAULT) {
        return handle_instruction_page_fault(address, info, return_pc);
    }
    return 1;
}

void kernel_exception_panic(void)
{
    uint64_t cause = cvm_exception_cause();
    uint64_t pc = cvm_exception_pc();
    uint64_t address = cvm_exception_address();
    uint64_t info = cvm_exception_info();

    kernel_uart_puts("KERNEL PANIC cause=");
    kernel_uart_put_hex64(cause);
    kernel_uart_puts(" epc=");
    kernel_uart_put_hex64(pc);
    kernel_uart_puts(" badaddr=");
    kernel_uart_put_hex64(address);
    kernel_uart_puts(" einfo=");
    kernel_uart_put_hex64(info);
    kernel_uart_puts("\n");
    cvm_halt();
}
