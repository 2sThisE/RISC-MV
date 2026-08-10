; CVM reference kernel bootstrap.
;
; Handoff:
;   R0 = CvmBootInfo physical address
;   R1 = CVM_BOOTINFO_HANDOFF_MAGIC
;   R2 = boot hardware-thread ID
;   supervisor mode, MMU off, interrupts disabled

.section .text
.global kernel_entry
.entry kernel_entry

kernel_entry:
    ; The whole 0x10000..0x1FFFF range belongs to the kernel image. Keeping
    ; the early stack inside that range prevents the PMM from reclaiming it.
    MOVI32U SP, 0x20000
    MOVI64 R3, kernel_boot_info
    STORE64 R3, R0

    CALLREL boot_info_validate
    CMPI32 R0, 0
    BRCC NE, kernel_boot_failed

    MOVI64 R0, message_boot_info_ok
    CALLREL uart_puts

    MOVI64 R0, kernel_boot_info
    LOAD64 R0, R0
    CALLREL pmm_init
    CMPI32 R0, 0
    BRCC NE, kernel_pmm_failed

    ; Verify alloc/free before the MMU consumes pages for its tables.
    CALLREL pmm_self_test
    CMPI32 R0, 0
    BRCC NE, kernel_pmm_failed
    MOVI64 R0, message_pmm_ok
    CALLREL uart_puts

    CALLREL bootstrap_mmu
    CMPI32 R0, 0
    BRCC NE, kernel_mmu_failed

    ; bootstrap_mmu changed the UART pointer to its low virtual alias before
    ; enabling translation, so this also verifies MMIO through the MMU.
    MOVI64 R0, message_mmu_ok
    CALLREL uart_puts

    ; The allocator free list lives in identity-mapped RAM and must remain
    ; usable after translation is enabled.
    CALLREL pmm_self_test
    CMPI32 R0, 0
    BRCC NE, kernel_runtime_failed

    CALLREL exception_init
    CMPI32 R0, 0
    BRCC NE, kernel_exception_failed
    MOVI64 R0, message_vbr_ok
    CALLREL uart_puts

    CALLREL memory_protection_self_test
    CMPI32 R0, 0
    BRCC NE, kernel_exception_failed
    MOVI64 R0, message_memory_protection_ok
    CALLREL uart_puts

    CALLREL demand_page_self_test
    CMPI32 R0, 0
    BRCC NE, kernel_exception_failed

    MOVI64 R0, message_ready
    CALLREL uart_puts
    MOVI32U R3, 42
    HALT

kernel_boot_failed:
    MOVI64 R0, message_boot_failed
    CALLREL uart_puts
    HALT

kernel_pmm_failed:
    MOVI64 R0, message_pmm_failed
    CALLREL uart_puts
    HALT

kernel_mmu_failed:
    MOVI64 R0, message_mmu_failed
    CALLREL uart_puts
    HALT

kernel_runtime_failed:
    MOVI64 R0, message_runtime_failed
    CALLREL uart_puts
    HALT

kernel_exception_failed:
    MOVI64 R0, message_exception_failed
    CALLREL uart_puts
    HALT

; ---------------------------------------------------------------------------
; BootInfo validation

; Returns R0=0 on success. The checksum field is restored before returning.
boot_info_validate:
    MOVI64 R3, kernel_boot_info
    LOAD64 R3, R3
    CMPI32 R3, 0
    BRCC EQ, boot_info_invalid

    MOVI64 R4, 0x31544F4F424D5643
    CMP R1, R4
    BRCC NE, boot_info_invalid
    LOAD64 R5, R3
    CMP R5, R4
    BRCC NE, boot_info_invalid

    LOAD16UO R4, R3, 0x08
    CMPI32 R4, 1
    BRCC NE, boot_info_invalid
    LOAD32UO R4, R3, 0x0C
    CMPI32 R4, 256
    BRCC NE, boot_info_invalid
    LOAD32UO R13, R3, 0x10
    CMPI32 R13, 256
    BRCC LTU, boot_info_invalid
    LOAD32UO R4, R3, 0x2C
    CMPI32 R4, 32
    BRCC NE, boot_info_invalid
    LOAD32UO R4, R3, 0xC8
    CMPI32 R4, 4096
    BRCC NE, boot_info_invalid

    ; The v1 loader enters with translation disabled.
    GETMMU R4
    CMPI32 R4, 0
    BRCC NE, boot_info_invalid

    ; Pick up the platform UART before reporting later stages.
    LOAD64O R4, R3, 0xA8
    CMPI32 R4, 0
    BRCC EQ, boot_info_invalid
    MOVI64 R5, kernel_uart_address
    STORE64 R5, R4

    ; CRC32(total_size), treating checksum at 0xF0 as zero.
    LOAD32UO R11, R3, 0xF0
    MOVI32U R4, 0
    STORE32O R3, R4, 0xF0
    MOV R12, R3
    CALLREL crc32
    MOVI64 R3, kernel_boot_info
    LOAD64 R3, R3
    STORE32O R3, R11, 0xF0
    CMP R0, R11
    BRCC NE, boot_info_invalid

    MOVI32U R0, 0
    RET

boot_info_invalid:
    MOVI32U R0, 1
    RET

; ---------------------------------------------------------------------------
; Physical page allocator
;
; Every CVM_MEMORY_USABLE page stores the next free physical page in its first
; eight bytes. The list needs no separately sized bitmap and therefore scales
; with RAM. bootstrap_mmu identity-maps RAM so the same allocator remains
; usable after MMUON.

; R0=BootInfo, returns R0=0 on success.
pmm_init:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14

    MOVI64 R1, pmm_free_head
    MOVI32U R2, 0
    STORE64 R1, R2
    MOVI64 R1, pmm_free_count
    STORE64 R1, R2

    LOAD64O R10, R0, 0x20
    ADD R10, R0
    LOAD32UO R11, R0, 0x28
    LOAD32UO R12, R0, 0x2C

pmm_map_loop:
    CMPI32 R11, 0
    BRCC EQ, pmm_init_done
    LOAD32UO R1, R10, 0x10
    CMPI32 R1, 1
    BRCC NE, pmm_next_entry

    LOAD64O R13, R10, 0x00
    LOAD64O R14, R10, 0x08
    MOV R2, R13
    ADD R2, R14
    CMP R2, R13
    BRCC LTU, pmm_init_fail

    ; start = align_up(base, 4096), end = align_down(base + length, 4096)
    ADDI32 R13, 4095
    CMP R13, R2
    BRCC GTU, pmm_next_entry
    MOVI64 R3, 0xFFFFFFFFFFFFF000
    AND R13, R3
    AND R2, R3

pmm_add_page_loop:
    CMP R13, R2
    BRCC GEU, pmm_next_entry
    MOVI64 R4, pmm_free_head
    LOAD64 R5, R4
    STORE64 R13, R5
    STORE64 R4, R13
    MOVI64 R4, pmm_free_count
    LOAD64 R5, R4
    ADDI32 R5, 1
    STORE64 R4, R5
    ADDI32 R13, 4096
    JUMPREL pmm_add_page_loop

pmm_next_entry:
    ADD R10, R12
    ADDI32 R11, -1
    JUMPREL pmm_map_loop

pmm_init_done:
    MOVI64 R1, pmm_free_head
    LOAD64 R1, R1
    CMPI32 R1, 0
    BRCC EQ, pmm_init_fail
    MOVI32U R0, 0
    JUMPREL pmm_init_return

pmm_init_fail:
    MOVI32U R0, 1
pmm_init_return:
    POP R14
    POP R13
    POP R12
    POP R11
    POP R10
    RET

; Returns one zeroed, 4KiB-aligned physical page in R0, or zero.
pmm_alloc_page:
    MOVI64 R1, pmm_free_head
    LOAD64 R3, R1
    CMPI32 R3, 0
    BRCC EQ, pmm_alloc_empty
    LOAD64 R2, R3
    STORE64 R1, R2
    MOVI64 R1, pmm_free_count
    LOAD64 R2, R1
    ADDI32 R2, -1
    STORE64 R1, R2

    MOV R4, R3
    MOVI32U R5, 512
    MOVI32U R6, 0
pmm_zero_page_loop:
    STORE64 R4, R6
    ADDI32 R4, 8
    ADDI32 R5, -1
    BRCC NE, pmm_zero_page_loop
    MOV R0, R3
    RET

pmm_alloc_empty:
    MOVI32U R0, 0
    RET

; R0=physical page. Callers only return pages obtained from pmm_alloc_page.
pmm_free_page:
    MOVI64 R1, pmm_free_head
    LOAD64 R2, R1
    STORE64 R0, R2
    STORE64 R1, R0
    MOVI64 R1, pmm_free_count
    LOAD64 R2, R1
    ADDI32 R2, 1
    STORE64 R1, R2
    RET

; Exercise LIFO reuse and a real page read/write. Returns R0=0 on success.
pmm_self_test:
    PUSH R10
    CALLREL pmm_alloc_page
    CMPI32 R0, 0
    BRCC EQ, pmm_self_test_fail
    MOV R10, R0
    MOVI64 R1, 0xC0DEC0DE55AA33CC
    STORE64 R10, R1
    LOAD64 R2, R10
    CMP R1, R2
    BRCC NE, pmm_self_test_release_fail
    MOV R0, R10
    CALLREL pmm_free_page
    CALLREL pmm_alloc_page
    CMP R0, R10
    BRCC NE, pmm_self_test_fail
    CALLREL pmm_free_page
    MOVI32U R0, 0
    JUMPREL pmm_self_test_return

pmm_self_test_release_fail:
    MOV R0, R10
    CALLREL pmm_free_page
pmm_self_test_fail:
    MOVI32U R0, 1
pmm_self_test_return:
    POP R10
    RET

; ---------------------------------------------------------------------------
; Bootstrap MMU

; Builds a 3-level, 4KiB page table. Complete RAM pages except page zero are
; initially identity mapped supervisor RW, then kernel sections are tightened
; to RX/R/RW. UART is mapped RW at 0x3FFFF000.
bootstrap_mmu:
    CALLREL pmm_alloc_page
    CMPI32 R0, 0
    BRCC EQ, bootstrap_mmu_fail
    MOVI64 R4, kernel_ptbr
    STORE64 R4, R0
    MOV R10, R0

    MOVI64 R4, kernel_boot_info
    LOAD64 R4, R4
    LOAD64O R11, R4, 0x38
    MOVI64 R5, 0xFFFFFFFFFFFFF000
    AND R11, R5
    ; Leave virtual page zero absent so NULL dereferences fault.
    MOVI32U R12, 4096

bootstrap_identity_loop:
    CMP R12, R11
    BRCC GEU, bootstrap_map_uart
    MOV R0, R10
    MOV R1, R12
    MOV R2, R12
    MOVI32U R3, 0x07
    CALLREL map_page
    CMPI32 R0, 0
    BRCC NE, bootstrap_mmu_fail
    ADDI32 R12, 4096
    JUMPREL bootstrap_identity_loop

bootstrap_map_uart:
    ; Enforce W^X on the kernel image before enabling translation.
    MOV R0, R10
    MOVI64 R1, kernel_entry
    MOVI64 R2, kernel_text_end
    MOVI32U R3, 0x0B
    CALLREL map_identity_range
    CMPI32 R0, 0
    BRCC NE, bootstrap_mmu_fail

    MOV R0, R10
    MOVI64 R1, kernel_rodata_start
    MOVI64 R2, kernel_rodata_end
    MOVI32U R3, 0x03
    CALLREL map_identity_range
    CMPI32 R0, 0
    BRCC NE, bootstrap_mmu_fail

    MOV R0, R10
    MOVI64 R1, kernel_data_start
    MOVI64 R2, kernel_data_end
    MOVI32U R3, 0x07
    CALLREL map_identity_range
    CMPI32 R0, 0
    BRCC NE, bootstrap_mmu_fail

    MOV R0, R10
    MOVI32U R1, 0x1F000
    MOVI32U R2, 0x20000
    MOVI32U R3, 0x07
    CALLREL map_identity_range
    CMPI32 R0, 0
    BRCC NE, bootstrap_mmu_fail

    MOV R0, R10
    MOVI32U R1, 0x3FFFF000
    MOVI64 R4, kernel_uart_address
    LOAD64 R2, R4
    MOVI32U R3, 0x07
    CALLREL map_page
    CMPI32 R0, 0
    BRCC NE, bootstrap_mmu_fail

    ; Switch the console pointer before MMUON, but do not access it until after.
    MOVI64 R4, kernel_uart_address
    MOVI32U R5, 0x3FFFF000
    STORE64 R4, R5
    MOVI64 R4, kernel_ptbr
    LOAD64 R4, R4
    SETPTBR R4
    MMUON
    GETMMU R5
    CMPI32 R5, 1
    BRCC NE, bootstrap_mmu_fail
    MOVI32U R0, 0
    RET

bootstrap_mmu_fail:
    MOVI32U R0, 1
    RET

; R0=root, R1=virtual, R2=physical, R3=leaf flags including VALID.
; Returns R0=0 on success. Intermediate tables contain VALID only.
map_page:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOV R10, R0
    MOV R11, R1
    MOV R12, R2
    MOV R13, R3
    MOVI64 R14, 0xFFFFFFFFFFFFF000

    ; Level 2 / virtual bits 38..30.
    MOV R4, R11
    SHR R4, 30
    ANDI32 R4, 0x1FF
    SHL R4, 3
    ADD R4, R10
    LOAD64 R5, R4
    TESTI32 R5, 1
    BRCC NE, map_have_level1
    MOV R8, R4
    CALLREL pmm_alloc_page
    CMPI32 R0, 0
    BRCC EQ, map_page_fail
    MOV R5, R0
    ORI32 R5, 1
    STORE64 R8, R5
map_have_level1:
    AND R5, R14

    ; Level 1 / virtual bits 29..21.
    MOV R4, R11
    SHR R4, 21
    ANDI32 R4, 0x1FF
    SHL R4, 3
    ADD R4, R5
    LOAD64 R6, R4
    TESTI32 R6, 1
    BRCC NE, map_have_level0
    MOV R8, R4
    CALLREL pmm_alloc_page
    CMPI32 R0, 0
    BRCC EQ, map_page_fail
    MOV R6, R0
    ORI32 R6, 1
    STORE64 R8, R6
map_have_level0:
    AND R6, R14

    ; Level 0 / virtual bits 20..12.
    MOV R4, R11
    SHR R4, 12
    ANDI32 R4, 0x1FF
    SHL R4, 3
    ADD R4, R6
    MOV R7, R12
    AND R7, R14
    OR R7, R13
    STORE64 R4, R7
    MOVI32U R0, 0
    JUMPREL map_page_return

map_page_fail:
    MOVI32U R0, 1
map_page_return:
    POP R14
    POP R13
    POP R12
    POP R11
    POP R10
    RET

; R0=root, R1=start, R2=end-exclusive, R3=leaf flags. Maps the physical pages
; at identical virtual addresses. Partial endpoints are rounded to pages.
map_identity_range:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOV R10, R0
    MOV R11, R1
    MOV R12, R2
    MOV R13, R3
    MOVI64 R14, 0xFFFFFFFFFFFFF000
    AND R11, R14
    ADDI32 R12, 4095
    AND R12, R14
map_identity_range_loop:
    CMP R11, R12
    BRCC GEU, map_identity_range_ok
    MOV R0, R10
    MOV R1, R11
    MOV R2, R11
    MOV R3, R13
    CALLREL map_page
    CMPI32 R0, 0
    BRCC NE, map_identity_range_fail
    ADDI32 R11, 4096
    JUMPREL map_identity_range_loop
map_identity_range_ok:
    MOVI32U R0, 0
    JUMPREL map_identity_range_return
map_identity_range_fail:
    MOVI32U R0, 1
map_identity_range_return:
    POP R14
    POP R13
    POP R12
    POP R11
    POP R10
    RET

; ---------------------------------------------------------------------------
; Exception vectors and handlers

; Allocate one permanent physical page for the 77-entry VBR table. Unused
; vectors remain UINT64_MAX. Synchronous exception vectors 65..75 initially
; use the common panic handler; LOAD_PAGE_FAULT has a demand-page handler.
exception_init:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14

    CALLREL pmm_alloc_page
    CMPI32 R0, 0
    BRCC EQ, exception_init_fail
    MOV R10, R0
    MOVI64 R1, kernel_vbr
    STORE64 R1, R10

    MOV R11, R10
    MOVI32U R12, 77
    MOVI64 R13, 0xFFFFFFFFFFFFFFFF
exception_fill_invalid:
    STORE64 R11, R13
    ADDI32 R11, 8
    ADDI32 R12, -1
    BRCC NE, exception_fill_invalid

    MOV R11, R10
    ADDI32 R11, 520
    MOVI32U R12, 11
    MOVI64 R13, exception_panic
exception_fill_sync:
    STORE64 R11, R13
    ADDI32 R11, 8
    ADDI32 R12, -1
    BRCC NE, exception_fill_sync

    MOV R11, R10
    ADDI32 R11, 576
    MOVI64 R13, instruction_page_fault_handler
    STORE64 R11, R13

    MOV R11, R10
    ADDI32 R11, 584
    MOVI64 R13, load_page_fault_handler
    STORE64 R11, R13

    MOV R11, R10
    ADDI32 R11, 592
    MOVI64 R13, store_page_fault_handler
    STORE64 R11, R13

    SETVBR R10
    GETVBR R11
    CMP R10, R11
    BRCC NE, exception_init_fail
    MOVI32U R11, 0x20000
    SETKSP R11
    MOVI32U R0, 0
    JUMPREL exception_init_return

exception_init_fail:
    MOVI32U R0, 1
exception_init_return:
    POP R14
    POP R13
    POP R12
    POP R11
    POP R10
    RET

; Exercise actual MMU enforcement. Dedicated handlers skip only these known
; probe instructions by editing the supervisor exception frame return PC.
memory_protection_self_test:
    MOVI64 R2, null_fault_count
    MOVI32U R1, 0
    STORE64 R2, R1
    MOVI64 R3, null_test_active
    MOVI32U R1, 1
    STORE64 R3, R1
    MOVI32U R0, 0
    MOVI32U R1, 0x12345678
null_load_probe:
    LOAD64 R1, R0
    CMPI32 R1, 0x12345678
    BRCC NE, memory_protection_self_test_fail
    LOAD64 R1, R2
    CMPI32 R1, 1
    BRCC NE, memory_protection_self_test_fail

    MOVI64 R2, store_fault_count
    MOVI32U R1, 0
    STORE64 R2, R1
    MOVI64 R3, store_test_active
    MOVI32U R1, 1
    STORE64 R3, R1
    MOVI64 R0, write_protect_probe
    MOVI32U R1, 0x41
write_store_probe:
    STORE8 R0, R1
    LOAD8U R1, R0
    CMPI32 R1, 0x5A
    BRCC NE, memory_protection_self_test_fail
    LOAD64 R1, R2
    CMPI32 R1, 1
    BRCC NE, memory_protection_self_test_fail

    MOVI64 R2, execute_fault_count
    MOVI32U R1, 0
    STORE64 R2, R1
    MOVI64 R3, execute_test_active
    MOVI32U R1, 1
    STORE64 R3, R1
    MOVI64 R0, execution_protect_probe
execute_call_probe:
    CALLR R0
    LOAD64 R1, R2
    CMPI32 R1, 1
    BRCC NE, memory_protection_self_test_fail
    MOVI32U R0, 0
    RET

memory_protection_self_test_fail:
    MOVI32U R0, 1
    RET

; Trigger a supervisor LOAD_PAGE_FAULT at a high unmapped virtual address.
; The handler maps a zeroed page and IRET retries this LOAD64 successfully.
demand_page_self_test:
    MOVI64 R0, 0x0000007FFFFFE000
    MOVI32U R1, 0
    MOVI64 R2, exception_recovery_count
    STORE64 R2, R1
    LOAD64 R1, R0
    CMPI32 R1, 0
    BRCC NE, demand_page_self_test_fail
    LOAD64 R1, R2
    CMPI32 R1, 1
    BRCC NE, demand_page_self_test_fail
    MOVI32U R0, 0
    RET
demand_page_self_test_fail:
    MOVI32U R0, 1
    RET

; Preserve every general register because IRET retries the faulting LOAD with
; its original address and destination operands.
load_page_fault_handler:
    PUSH R0
    PUSH R1
    PUSH R2
    PUSH R3
    PUSH R4
    PUSH R5
    PUSH R6
    PUSH R7
    PUSH R8
    PUSH R9
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14

    EXCAUSE R10
    CMPI32 R10, 9
    BRCC NE, load_page_fault_fatal
    EXADDR R11
    CMPI32 R11, 0
    BRCC EQ, null_page_fault_probe
    MOVI64 R12, 0x0000007FFFFFE000
    CMP R11, R12
    BRCC NE, load_page_fault_fatal
    EXINFO R12
    TESTI32 R12, 1
    BRCC EQ, load_page_fault_fatal
    TESTI32 R12, 2
    BRCC EQ, load_page_fault_fatal
    TESTI32 R12, 0x40
    BRCC EQ, load_page_fault_fatal
    TESTI32 R12, 0x400
    BRCC EQ, load_page_fault_fatal

    CALLREL pmm_alloc_page
    CMPI32 R0, 0
    BRCC EQ, load_page_fault_fatal
    MOV R13, R0
    MOVI64 R14, demand_test_physical
    STORE64 R14, R13

    MOVI64 R0, kernel_ptbr
    LOAD64 R0, R0
    MOV R1, R11
    MOV R2, R13
    MOVI32U R3, 0x07
    CALLREL map_page
    CMPI32 R0, 0
    BRCC NE, load_page_fault_fatal

    MOVI64 R0, exception_recovery_count
    LOAD64 R1, R0
    ADDI32 R1, 1
    STORE64 R0, R1
    MOVI64 R0, message_page_fault_recovered
    CALLREL uart_puts
    JUMPREL exception_restore_all_iret

null_page_fault_probe:
    EXINFO R12
    TESTI32 R12, 1
    BRCC EQ, load_page_fault_fatal
    TESTI32 R12, 2
    BRCC EQ, load_page_fault_fatal
    TESTI32 R12, 0x40
    BRCC EQ, load_page_fault_fatal
    TESTI32 R12, 0x400
    BRCC EQ, load_page_fault_fatal
    MOVI64 R0, null_test_active
    LOAD64 R1, R0
    CMPI32 R1, 1
    BRCC NE, load_page_fault_fatal
    MOVI32U R1, 0
    STORE64 R0, R1

    ; After fifteen saved GPRs, the supervisor exception frame starts at
    ; SP+120 and its first qword is the retry PC.
    MOV R4, SP
    ADDI32 R4, 120
    LOAD64 R5, R4
    MOVI64 R6, null_load_probe
    CMP R5, R6
    BRCC NE, load_page_fault_fatal
    ADDI32 R5, 3
    STORE64 R4, R5
    MOVI64 R0, null_fault_count
    LOAD64 R1, R0
    ADDI32 R1, 1
    STORE64 R0, R1
    MOVI64 R0, message_null_blocked
    CALLREL uart_puts
    JUMPREL exception_restore_all_iret

load_page_fault_fatal:
    JUMPREL exception_panic

; STORE_PAGE_FAULT used by the rodata write-protection probe.
store_page_fault_handler:
    PUSH R0
    PUSH R1
    PUSH R2
    PUSH R3
    PUSH R4
    PUSH R5
    PUSH R6
    PUSH R7
    PUSH R8
    PUSH R9
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    EXCAUSE R10
    CMPI32 R10, 10
    BRCC NE, store_page_fault_fatal
    EXADDR R11
    MOVI64 R12, write_protect_probe
    CMP R11, R12
    BRCC NE, store_page_fault_fatal
    EXINFO R12
    TESTI32 R12, 1
    BRCC EQ, store_page_fault_fatal
    TESTI32 R12, 4
    BRCC EQ, store_page_fault_fatal
    TESTI32 R12, 0x40
    BRCC EQ, store_page_fault_fatal
    TESTI32 R12, 0x1000
    BRCC EQ, store_page_fault_fatal
    MOVI64 R0, store_test_active
    LOAD64 R1, R0
    CMPI32 R1, 1
    BRCC NE, store_page_fault_fatal
    MOVI32U R1, 0
    STORE64 R0, R1
    MOV R4, SP
    ADDI32 R4, 120
    LOAD64 R5, R4
    MOVI64 R6, write_store_probe
    CMP R5, R6
    BRCC NE, store_page_fault_fatal
    ADDI32 R5, 3
    STORE64 R4, R5
    MOVI64 R0, store_fault_count
    LOAD64 R1, R0
    ADDI32 R1, 1
    STORE64 R0, R1
    MOVI64 R0, message_write_blocked
    CALLREL uart_puts
    JUMPREL exception_restore_all_iret
store_page_fault_fatal:
    JUMPREL exception_panic

; INSTRUCTION_PAGE_FAULT used by the data-page execution probe.
instruction_page_fault_handler:
    PUSH R0
    PUSH R1
    PUSH R2
    PUSH R3
    PUSH R4
    PUSH R5
    PUSH R6
    PUSH R7
    PUSH R8
    PUSH R9
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    EXCAUSE R10
    CMPI32 R10, 8
    BRCC NE, instruction_page_fault_fatal
    EXADDR R11
    MOVI64 R12, execution_protect_probe
    CMP R11, R12
    BRCC NE, instruction_page_fault_fatal
    EXINFO R12
    TESTI32 R12, 1
    BRCC EQ, instruction_page_fault_fatal
    TESTI32 R12, 8
    BRCC EQ, instruction_page_fault_fatal
    TESTI32 R12, 0x40
    BRCC EQ, instruction_page_fault_fatal
    TESTI32 R12, 0x1000
    BRCC EQ, instruction_page_fault_fatal
    MOVI64 R0, execute_test_active
    LOAD64 R1, R0
    CMPI32 R1, 1
    BRCC NE, instruction_page_fault_fatal
    MOVI32U R1, 0
    STORE64 R0, R1
    MOV R4, SP
    ADDI32 R4, 120
    LOAD64 R5, R4
    MOVI64 R6, execute_call_probe
    CMP R5, R6
    BRCC NE, instruction_page_fault_fatal
    ADDI32 R5, 2
    STORE64 R4, R5
    MOVI64 R0, execute_fault_count
    LOAD64 R1, R0
    ADDI32 R1, 1
    STORE64 R0, R1
    MOVI64 R0, message_execute_blocked
    CALLREL uart_puts
    JUMPREL exception_restore_all_iret
instruction_page_fault_fatal:
    JUMPREL exception_panic

exception_restore_all_iret:
    POP R14
    POP R13
    POP R12
    POP R11
    POP R10
    POP R9
    POP R8
    POP R7
    POP R6
    POP R5
    POP R4
    POP R3
    POP R2
    POP R1
    POP R0
    IRET

; All non-recoverable synchronous exceptions report the architectural state
; and stop this boot thread in a controlled kernel panic.
exception_panic:
    EXCAUSE R10
    EXPC R11
    EXADDR R12
    EXINFO R13

    MOVI64 R0, message_panic
    CALLREL uart_puts
    MOVI64 R0, message_cause
    CALLREL uart_puts
    MOV R0, R10
    CALLREL uart_put_hex64
    MOVI64 R0, message_epc
    CALLREL uart_puts
    MOV R0, R11
    CALLREL uart_put_hex64
    MOVI64 R0, message_badaddr
    CALLREL uart_puts
    MOV R0, R12
    CALLREL uart_put_hex64
    MOVI64 R0, message_einfo
    CALLREL uart_puts
    MOV R0, R13
    CALLREL uart_put_hex64
    MOVI64 R0, message_newline
    CALLREL uart_puts
    HALT

; ---------------------------------------------------------------------------
; Runtime helpers

; R12=data, R13=size, returns CRC32 in R0.
crc32:
    MOVI64 R2, 0xFFFFFFFF
crc32_byte_loop:
    CMPI32 R13, 0
    BRCC EQ, crc32_done
    LOAD8U R3, R12
    XOR R2, R3
    MOVI32U R4, 8
crc32_bit_loop:
    MOV R5, R2
    ANDI32 R5, 1
    SHR R2, 1
    CMPI32 R5, 0
    BRCC EQ, crc32_bit_next
    XORI32 R2, 0xEDB88320
crc32_bit_next:
    ADDI32 R4, -1
    BRCC NE, crc32_bit_loop
    ADDI32 R12, 1
    ADDI32 R13, -1
    JUMPREL crc32_byte_loop
crc32_done:
    NOT R2
    ZEXT32 R0, R2
    RET

; R0=NUL-terminated string. The UART pointer is physical before MMUON and a
; low virtual alias afterwards.
uart_puts:
    MOVI64 R2, kernel_uart_address
    LOAD64 R2, R2
uart_puts_loop:
    LOAD8U R1, R0
    CMPI32 R1, 0
    BRCC EQ, uart_puts_done
    STORE8 R2, R1
    ADDI32 R0, 1
    JUMPREL uart_puts_loop
uart_puts_done:
    RET

; R0=value. Prints exactly sixteen uppercase hexadecimal digits.
uart_put_hex64:
    MOVI64 R2, kernel_uart_address
    LOAD64 R2, R2
    MOVI32U R3, 16
uart_put_hex_loop:
    MOV R4, R0
    SHR R4, 60
    MOVI64 R5, hex_digits
    ADD R5, R4
    LOAD8U R4, R5
    STORE8 R2, R4
    SHL R0, 4
    ADDI32 R3, -1
    BRCC NE, uart_put_hex_loop
    RET

; ---------------------------------------------------------------------------
; Page-separated kernel sections. The single-segment image container remains
; a bootstrap limitation, but runtime PTEs enforce RX/R/RW on these pages.

kernel_text_end:

.section .rodata
kernel_rodata_start:

message_boot_info_ok:
    .asciz "KERNEL: BootInfo OK\n"
message_pmm_ok:
    .asciz "KERNEL: PMM OK\n"
message_mmu_ok:
    .asciz "KERNEL: MMU ON\n"
message_vbr_ok:
    .asciz "KERNEL: VBR OK\n"
message_page_fault_recovered:
    .asciz "KERNEL: PAGE FAULT RECOVERED\n"
message_null_blocked:
    .asciz "KERNEL: NULL PAGE BLOCKED\n"
message_write_blocked:
    .asciz "KERNEL: WRITE PROTECT OK\n"
message_execute_blocked:
    .asciz "KERNEL: NX PROTECT OK\n"
message_memory_protection_ok:
    .asciz "KERNEL: MEMORY PROTECTION OK\n"
message_ready:
    .asciz "KERNEL: READY\n"
message_boot_failed:
    .asciz "KERNEL ERROR: BootInfo\n"
message_pmm_failed:
    .asciz "KERNEL ERROR: PMM\n"
message_mmu_failed:
    .asciz "KERNEL ERROR: MMU\n"
message_runtime_failed:
    .asciz "KERNEL ERROR: runtime PMM\n"
message_exception_failed:
    .asciz "KERNEL ERROR: exception self-test\n"
message_panic:
    .asciz "KERNEL PANIC"
message_cause:
    .asciz " cause="
message_epc:
    .asciz " epc="
message_badaddr:
    .asciz " badaddr="
message_einfo:
    .asciz " einfo="
message_newline:
    .asciz "\n"
hex_digits:
    .ascii "0123456789ABCDEF"
write_protect_probe:
    .byte 0x5A
kernel_rodata_end:

.section .data
kernel_data_start:
.align 8
kernel_boot_info:
    .qword 0
kernel_uart_address:
    .qword 0xFFFFFFFFFFFD0000
pmm_free_head:
    .qword 0
pmm_free_count:
    .qword 0
kernel_ptbr:
    .qword 0
kernel_vbr:
    .qword 0
exception_recovery_count:
    .qword 0
demand_test_physical:
    .qword 0
null_test_active:
    .qword 0
null_fault_count:
    .qword 0
store_test_active:
    .qword 0
store_fault_count:
    .qword 0
execute_test_active:
    .qword 0
execute_fault_count:
    .qword 0
execution_protect_probe:
    .qword 0
kernel_data_end:

.section .bss
kernel_bss_start:
    ; Keep the bootstrap stack and its guardable page inside the kernel's
    ; reserved aggregate range while emitting no bytes into KERNEL.CVM.
    .space 0x20000 - kernel_bss_start
kernel_bss_end:
