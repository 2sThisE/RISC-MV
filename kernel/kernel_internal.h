#ifndef CVM_KERNEL_INTERNAL_H
#define CVM_KERNEL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "boot_format.h"

#define KERNEL_PAGE_SHIFT 12U
#define KERNEL_PAGE_SIZE UINT64_C(4096)
#define KERNEL_PAGE_MASK (KERNEL_PAGE_SIZE - UINT64_C(1))
#define KERNEL_PTE_ADDRESS_MASK (~KERNEL_PAGE_MASK)

#define KERNEL_PTE_VALID   (UINT64_C(1) << 0)
#define KERNEL_PTE_READ    (UINT64_C(1) << 1)
#define KERNEL_PTE_WRITE   (UINT64_C(1) << 2)
#define KERNEL_PTE_EXECUTE (UINT64_C(1) << 3)

#define KERNEL_VECTOR_ENTRY_COUNT 77U
#define KERNEL_VECTOR_EXCEPTION_BASE 64U

#define KERNEL_EXCEPTION_INSTRUCTION_PAGE_FAULT UINT64_C(8)
#define KERNEL_EXCEPTION_LOAD_PAGE_FAULT UINT64_C(9)
#define KERNEL_EXCEPTION_STORE_PAGE_FAULT UINT64_C(10)

#define KERNEL_EINFO_BADADDR_VALID  (UINT64_C(1) << 0)
#define KERNEL_EINFO_ACCESS_READ    (UINT64_C(1) << 1)
#define KERNEL_EINFO_ACCESS_WRITE   (UINT64_C(1) << 2)
#define KERNEL_EINFO_ACCESS_EXECUTE (UINT64_C(1) << 3)
#define KERNEL_EINFO_MMU_ENABLED    (UINT64_C(1) << 6)
#define KERNEL_EINFO_NOT_PRESENT    (UINT64_C(1) << 10)
#define KERNEL_EINFO_PERMISSION     (UINT64_C(1) << 12)

#define KERNEL_UART_ALIAS UINT64_C(0x3FFFF000)
#define KERNEL_DEMAND_TEST_ADDRESS UINT64_C(0x0000007FFFFFE000)
#define KERNEL_EXCEPTION_STACK_TOP UINT64_C(0x20000)

extern CvmBootInfo *kernel_boot_info;
extern uintptr_t kernel_uart_address;

extern uint8_t kernel_text_start[];
extern uint8_t kernel_text_end[];
extern uint8_t kernel_rodata_start[];
extern uint8_t kernel_rodata_end[];
extern uint8_t kernel_data_start[];
extern uint8_t kernel_data_end[];
extern uint8_t kernel_stack_bottom[];
extern uint8_t kernel_stack_top[];

void kernel_uart_puts(const char *text);
void kernel_uart_put_hex64(uint64_t value);

int kernel_pmm_init(const CvmBootInfo *info);
uintptr_t kernel_pmm_alloc_page(void);
void kernel_pmm_free_page(uintptr_t page);
int kernel_pmm_self_test(void);

int kernel_map_page(uintptr_t root, uintptr_t virtual_address,
                    uintptr_t physical_address, uint64_t flags);
int kernel_bootstrap_mmu(void);
uintptr_t kernel_page_table_root(void);

int kernel_exception_init(void);
int kernel_memory_protection_self_test(void);
int kernel_demand_page_self_test(void);
int kernel_handle_page_fault(uint64_t *return_pc);
void kernel_exception_panic(void);

extern void kernel_exception_panic_entry(void);
extern void kernel_page_fault_entry(void);
extern int kernel_probe_null_load(void);
extern int kernel_probe_rodata_store(void);
extern int kernel_probe_data_execute(void);
extern uint8_t kernel_null_load_probe_instruction[];
extern uint8_t kernel_null_load_probe_resume[];
extern uint8_t kernel_store_probe_instruction[];
extern uint8_t kernel_store_probe_resume[];
extern uint8_t kernel_execute_probe_instruction[];
extern uint8_t kernel_execute_probe_resume[];
extern const uint8_t kernel_write_protect_probe;
extern uint64_t kernel_execution_protect_probe;

#endif
