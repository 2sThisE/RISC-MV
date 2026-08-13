#ifndef CVM_KERNEL_INTERNAL_H
#define CVM_KERNEL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "boot_format.h"
#include "kernel_runtime.h"
#include "rarchm64_syscall.h"

#define KERNEL_PAGE_SHIFT 12U
#define KERNEL_PAGE_SIZE UINT64_C(4096)
#define KERNEL_PAGE_MASK (KERNEL_PAGE_SIZE - UINT64_C(1))
#define KERNEL_LARGE_PAGE_SHIFT 21U
#define KERNEL_LARGE_PAGE_SIZE (UINT64_C(1) << KERNEL_LARGE_PAGE_SHIFT)
#define KERNEL_LARGE_PAGE_MASK (KERNEL_LARGE_PAGE_SIZE - UINT64_C(1))
#define KERNEL_GIGA_PAGE_SHIFT 30U
#define KERNEL_GIGA_PAGE_SIZE (UINT64_C(1) << KERNEL_GIGA_PAGE_SHIFT)
#define KERNEL_GIGA_PAGE_MASK (KERNEL_GIGA_PAGE_SIZE - UINT64_C(1))
#define KERNEL_PTE_ADDRESS_MASK (~KERNEL_PAGE_MASK)

#define KERNEL_PTE_VALID   (UINT64_C(1) << 0)
#define KERNEL_PTE_READ    (UINT64_C(1) << 1)
#define KERNEL_PTE_WRITE   (UINT64_C(1) << 2)
#define KERNEL_PTE_EXECUTE (UINT64_C(1) << 3)
#define KERNEL_PTE_USER    (UINT64_C(1) << 4)
#define KERNEL_PTE_PERMISSION_MASK \
    (KERNEL_PTE_READ | KERNEL_PTE_WRITE | KERNEL_PTE_EXECUTE)

#define KERNEL_VECTOR_ENTRY_COUNT 77U
#define KERNEL_VECTOR_EXCEPTION_BASE 64U
#define KERNEL_VECTOR_SYSCALL 76U
#define KERNEL_TIMER_INTERRUPT_LINE 0U

#define KERNEL_SAVED_USER_MODE (UINT64_C(1) << 62)
#define KERNEL_SAVED_EXCEPTION_FRAME (UINT64_C(1) << 63)
#define KERNEL_CPU_FLAG_INTERRUPT_ENABLE (UINT64_C(1) << 4)

#define KERNEL_EXCEPTION_INSTRUCTION_PAGE_FAULT UINT64_C(8)
#define KERNEL_EXCEPTION_LOAD_PAGE_FAULT UINT64_C(9)
#define KERNEL_EXCEPTION_STORE_PAGE_FAULT UINT64_C(10)

#define KERNEL_ERROR_BAD_FD RARCHM64_EBADF
#define KERNEL_ERROR_NO_MEMORY RARCHM64_ENOMEM
#define KERNEL_ERROR_ACCESS RARCHM64_EACCES
#define KERNEL_ERROR_NO_CHILD RARCHM64_ECHILD
#define KERNEL_ERROR_AGAIN RARCHM64_EAGAIN
#define KERNEL_ERROR_FAULT RARCHM64_EFAULT
#define KERNEL_ERROR_BUSY RARCHM64_EBUSY
#define KERNEL_ERROR_INVALID RARCHM64_EINVAL
#define KERNEL_ERROR_TOO_MANY_FILES RARCHM64_EMFILE
#define KERNEL_ERROR_TOO_BIG RARCHM64_EFBIG
#define KERNEL_ERROR_IO RARCHM64_EIO
#define KERNEL_ERROR_NO_ENTRY RARCHM64_ENOENT
#define KERNEL_ERROR_EXEC_FORMAT RARCHM64_ENOEXEC
#define KERNEL_ERROR_DEADLOCK RARCHM64_EDEADLK
#define KERNEL_ERROR_NOT_IMPLEMENTED RARCHM64_ENOSYS

#define KERNEL_EINFO_BADADDR_VALID  (UINT64_C(1) << 0)
#define KERNEL_EINFO_ACCESS_READ    (UINT64_C(1) << 1)
#define KERNEL_EINFO_ACCESS_WRITE   (UINT64_C(1) << 2)
#define KERNEL_EINFO_ACCESS_EXECUTE (UINT64_C(1) << 3)
#define KERNEL_EINFO_ORIGIN_USER    (UINT64_C(1) << 5)
#define KERNEL_EINFO_MMU_ENABLED    (UINT64_C(1) << 6)
#define KERNEL_EINFO_NOT_PRESENT    (UINT64_C(1) << 10)
#define KERNEL_EINFO_PERMISSION     (UINT64_C(1) << 12)

#define KERNEL_VIRTUAL_BASE UINT64_C(0x40000000)
#define KERNEL_UART_ALIAS UINT64_C(0x3FFFF000)
#define KERNEL_VIO_ALIAS UINT64_C(0x3F000000)
#define KERNEL_IRQ_ALIAS UINT64_C(0x3F001000)
#define KERNEL_TIMER_ALIAS UINT64_C(0x3F002000)
#define KERNEL_EXTERNAL_MMIO_BASE UINT64_C(0x3F100000)
#define KERNEL_EXTERNAL_MMIO_STRIDE UINT64_C(0x10000)
#define KERNEL_DIRECT_MAP_BASE UINT64_C(0x100000000)
#define KERNEL_DEMAND_TEST_ADDRESS UINT64_C(0x0000007FFFFFE000)
#define KERNEL_EXCEPTION_STACK_TOP UINT64_C(0x20000)

extern CvmBootInfo *kernel_boot_info;
extern CvmBootVirtualHandoff kernel_virtual_handoff;
extern uintptr_t kernel_uart_address;

extern uint8_t kernel_text_start[];
extern uint8_t kernel_text_end[];
extern uint8_t kernel_rodata_start[];
extern uint8_t kernel_rodata_end[];
extern uint8_t kernel_data_start[];
extern uint8_t kernel_data_end[];
extern uint8_t kernel_bss_start[];
extern uint8_t kernel_bss_end[];
extern uint8_t kernel_stack_bottom[];
extern uint8_t kernel_stack_top[];

void kernel_uart_puts(const char *text);
void kernel_uart_put_hex64(uint64_t value);

int kernel_pmm_init(const CvmBootInfo *info);
uintptr_t kernel_pmm_alloc_page(void);
void kernel_pmm_free_page(uintptr_t page);
uint64_t kernel_pmm_free_page_count(void);
void kernel_pmm_release_range(uintptr_t base, uintptr_t size);
int kernel_pmm_self_test(void);

int kernel_heap_init(void);
void *kernel_malloc(size_t size);
void *kernel_calloc(size_t count, size_t size);
void kernel_free(void *pointer);
int kernel_heap_validate(void);
int kernel_heap_self_test(void);

int kernel_devices_init(void);
int kernel_devices_self_test(void);
int kernel_devices_enable_interrupts(void);
int kernel_devices_runtime_valid(void);
void kernel_devices_interrupt(uint64_t *frame);
int kernel_block_read(uint64_t lba, void *buffer, size_t sector_count);
int kernel_block_write(uint64_t lba, const void *buffer, size_t sector_count);
int kernel_block_flush(void);
uint64_t kernel_block_capacity(void);
int kernel_block_read_only(void);
int kernel_keyboard_poll(uint64_t *event);
int kernel_keyboard_read(uint64_t *frame,
                         uintptr_t user_buffer,
                         size_t size,
                         int64_t *result);
int kernel_display_present_test_pattern(void);

int kernel_vfs_init(void);
int kernel_vfs_read_file(const char *path,
                         void *buffer,
                         size_t capacity,
                         size_t *file_size);
int kernel_vfs_read_all(const char *path,
                        size_t maximum_size,
                        uint8_t **data,
                        size_t *size);
int kernel_vfs_write_file(const char *path,
                           const void *buffer,
                           size_t size);
int kernel_vfs_sync(void);
int kernel_vfs_self_test(void);

#define KERNEL_FD_LIMIT 32
#define KERNEL_VFS_OPEN_READ RARCHM64_O_READ
#define KERNEL_VFS_OPEN_WRITE RARCHM64_O_WRITE
#define KERNEL_VFS_OPEN_APPEND RARCHM64_O_APPEND

typedef enum {
    KERNEL_VNODE_REGULAR = 0,
    KERNEL_VNODE_KEYBOARD = 1,
    KERNEL_VNODE_UART = 2
} KernelVnodeKind;

KernelOpenFile *kernel_vfs_open(const char *path, uint32_t flags);
void kernel_vfs_file_retain(KernelOpenFile *file);
void kernel_vfs_file_release(KernelOpenFile *file);
KernelFdTable *kernel_fd_table_create(void);
void kernel_fd_table_destroy(KernelFdTable *table);
int kernel_fd_install(KernelFdTable *table,
                      KernelOpenFile *file,
                      int minimum_fd);
KernelOpenFile *kernel_fd_acquire(KernelFdTable *table, int fd);
int kernel_fd_close(KernelFdTable *table, int fd);
size_t kernel_fd_open_count(KernelFdTable *table);
int kernel_fd_populate_standard(KernelFdTable *table);
int64_t kernel_vfs_file_read(KernelOpenFile *file, void *buffer, size_t size);
int64_t kernel_vfs_file_write(KernelOpenFile *file,
                              const void *buffer,
                              size_t size);
int64_t kernel_vfs_file_seek(KernelOpenFile *file,
                               int64_t offset,
                               uint32_t whence);
int kernel_vfs_file_sync(KernelOpenFile *file);
KernelVnodeKind kernel_vfs_file_kind(KernelOpenFile *file);

int kernel_copy_from_user(const KernelAddressSpace *space,
                          void *destination,
                          uintptr_t source,
                          size_t size);
int kernel_copy_to_user(const KernelAddressSpace *space,
                        uintptr_t destination,
                        const void *source,
                        size_t size);
int kernel_user_buffer_writable(const KernelAddressSpace *space,
                                uintptr_t destination,
                                size_t size);
int kernel_syscall_self_test(void);
void kernel_syscall_dispatch(uint64_t *frame);

KernelAddressSpace *kernel_scheduler_current_space(void);
KernelFdTable *kernel_scheduler_current_fd_table(void);
uint64_t kernel_scheduler_current_pid(void);
void kernel_scheduler_yield(uint64_t *frame);
void kernel_wait_queue_init(KernelWaitQueue *queue);
size_t kernel_wait_queue_wake_one(KernelWaitQueue *queue, int64_t result);
size_t kernel_wait_queue_wake_all(KernelWaitQueue *queue, int64_t result);
size_t kernel_wait_queue_wake_read_one(KernelWaitQueue *queue,
                                       const void *data,
                                       size_t size);
int kernel_scheduler_block_current(uint64_t *frame,
                                   KernelWaitQueue *queue,
                                   uint64_t timeout_ticks,
                                   int64_t timeout_result);
int kernel_scheduler_block_read(uint64_t *frame,
                                KernelWaitQueue *queue,
                                uintptr_t buffer_address,
                                size_t buffer_size);
int kernel_scheduler_block_kernel(KernelWaitQueue *queue,
                                  uint64_t timeout_ticks,
                                  int64_t timeout_result,
                                  int64_t *wake_result);
void kernel_scheduler_syscall_enter(uint64_t *frame);
void kernel_scheduler_syscall_leave(uint64_t *frame);
int kernel_scheduler_termination_requested(int64_t *status);
int kernel_scheduler_running(void);
uint64_t kernel_scheduler_ticks(void);
void kernel_scheduler_interrupt_return(uint64_t *frame);
int kernel_scheduler_sleep(uint64_t *frame,
                           uint64_t milliseconds,
                           int64_t *result);
int kernel_scheduler_waitpid(uint64_t *frame,
                             uint64_t pid,
                             uintptr_t status_address,
                             int64_t *result);
int kernel_scheduler_join(uint64_t *frame,
                          uint64_t tid,
                          uintptr_t status_address,
                          int64_t *result);
int kernel_scheduler_exec(uint64_t *frame,
                          const char *path,
                          size_t argument_count,
                          const char *const *arguments,
                          size_t environment_count,
                          const char *const *environment,
                          int64_t *result);
int kernel_scheduler_register_process(KernelProcess *process);
void kernel_scheduler_exit(uint64_t *frame, int64_t status);
void kernel_scheduler_fault(uint64_t *frame,
                            uint64_t cause,
                            uint64_t address,
                            uint64_t info);
void kernel_scheduler_note_write(void);
void kernel_scheduler_timer(uint64_t *frame);
int kernel_scheduler_self_test(void);

void *kernel_phys_to_virt(uintptr_t physical_address);
uintptr_t kernel_virt_to_phys(const void *virtual_address);

int kernel_map_page(uintptr_t root, uintptr_t virtual_address,
                    uintptr_t physical_address, uint64_t flags);
int kernel_bootstrap_mmu(void);
uintptr_t kernel_page_table_root(void);

int kernel_exception_init(void);
int kernel_memory_protection_self_test(void);
int kernel_demand_page_self_test(void);
int kernel_handle_page_fault(uint64_t *return_pc);
int kernel_exception_dispatch(uint64_t *frame);
void kernel_exception_panic(void);

extern void kernel_exception_panic_entry(void);
extern void kernel_exception_entry(void);
extern void kernel_page_fault_entry(void);
extern void kernel_syscall_entry(void);
extern void kernel_timer_entry(void);
extern void kernel_device_irq_entry(void);
extern void kernel_idle_wait(void);
extern uint8_t kernel_idle_wait_instruction[];
extern uint8_t kernel_idle_wait_resume[];
extern void kernel_suspend_to_user(uintptr_t *resume_sp,
                                   uint64_t page_table_root,
                                   uint64_t entry,
                                   uint64_t user_stack_pointer,
                                   uint64_t kernel_stack_pointer,
                                   const uint64_t *initial_registers);
extern void kernel_switch_continuation(uintptr_t *current_resume_sp,
                                       uintptr_t next_resume_sp);
extern void kernel_resume_continuation(uintptr_t resume_sp);
extern void kernel_start_user(uint64_t page_table_root,
                              uint64_t entry,
                              uint64_t stack_pointer,
                              uint64_t kernel_stack_pointer,
                              const uint64_t *initial_registers);
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
