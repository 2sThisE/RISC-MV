; CVM reference kernel architecture boundary.
;
; Ordinary kernel policy lives in C. This file only establishes the initial
; stack, preserves the architectural exception context around C handlers, and
; contains the exact faulting instructions used by the protection self-test.

.section .text
.global kernel_entry
.global kernel_exception_panic_entry
.global kernel_exception_entry
.global kernel_page_fault_entry
.global kernel_syscall_entry
.global kernel_timer_entry
.global kernel_start_user
.global kernel_probe_null_load
.global kernel_probe_rodata_store
.global kernel_probe_data_execute
.global kernel_null_load_probe_instruction
.global kernel_null_load_probe_resume
.global kernel_store_probe_instruction
.global kernel_store_probe_resume
.global kernel_execute_probe_instruction
.global kernel_execute_probe_resume
.extern kernel_main
.extern kernel_exception_dispatch
.extern kernel_exception_panic
.extern kernel_syscall_dispatch
.extern kernel_scheduler_timer
.extern kernel_stack_top
.type kernel_main, function
.type kernel_exception_dispatch, function
.type kernel_exception_panic, function
.entry kernel_entry

; Handoff registers are already R0=BootInfo, R1=magic, R2=boot thread ID.
kernel_entry:
    MOVI64 SP, kernel_stack_top
    CALLREL kernel_main
    MOV R3, R0
    HALT

; Non-recoverable exception entry. The CPU exception frame made SP aligned for
; a direct C call. kernel_exception_panic never returns.
kernel_exception_panic_entry:
    CALLREL kernel_exception_panic
    HALT

; Every synchronous exception uses this wrapper. Preserve every GPR because a
; supervisor fault may retry the instruction and a user fault may switch to a
; different thread. Fifteen pushes require one padding qword for ABI alignment.
kernel_exception_entry:
kernel_page_fault_entry:
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
    ADDI32 SP, -8

    MOV R0, SP
    CALLREL kernel_exception_dispatch
    CMPI32 R0, 0
    BRCC NE, kernel_page_fault_fatal

    ADDI32 SP, 8
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

kernel_page_fault_fatal:
    CALLREL kernel_exception_panic
    HALT

; A user trap frame is followed by the same fixed GPR save area for syscalls
; and timer interrupts. C may replace the saved registers and CPU frame to
; perform a context switch before this wrapper restores them with IRET.
kernel_syscall_entry:
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
    ADDI32 SP, -8
    MOV R0, SP
    CALLREL kernel_syscall_dispatch
    JUMPREL kernel_trap_restore

kernel_timer_entry:
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
    ADDI32 SP, -8
    MOV R0, SP
    CALLREL kernel_scheduler_timer

kernel_trap_restore:
    ADDI32 SP, 8
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

; R0=root, R1=entry, R2=user SP, R3=kernel SP, R4=initial R0..R14 array.
; Kernel mappings are shared by every user address space, so execution remains
; valid across SETPTBR.
kernel_start_user:
    MOV R14, R1
    MOV R13, R2
    MOV R11, R4
    SETPTBR R0
    SETKSP R3
    MOV SP, R3
    PUSH R13
    MOVI64 R12, 0x4000000000000010
    PUSH R12
    PUSH R14
    LOAD64O R0, R11, 0
    LOAD64O R1, R11, 8
    LOAD64O R2, R11, 16
    LOAD64O R3, R11, 24
    LOAD64O R4, R11, 32
    LOAD64O R5, R11, 40
    LOAD64O R6, R11, 48
    LOAD64O R7, R11, 56
    LOAD64O R8, R11, 64
    LOAD64O R9, R11, 72
    LOAD64O R10, R11, 80
    LOAD64O R13, R11, 104
    LOAD64O R14, R11, 112
    LOAD64O R12, R11, 96
    LOAD64O R11, R11, 88
    IRET
    HALT

; The C handler replaces the saved retry PC with the exported resume label.
kernel_probe_null_load:
    ; A normal callee enters with SP%16==8. Reserve one qword so a faulting
    ; instruction is observed with the same aligned SP as compiler C code.
    ADDI32 SP, -8
    MOVI32U R0, 0
    MOVI32U R1, 0x12345678
kernel_null_load_probe_instruction:
    LOAD64 R1, R0
kernel_null_load_probe_resume:
    CMPI32 R1, 0x12345678
    BRCC NE, kernel_probe_failed
    MOVI32U R0, 0
    ADDI32 SP, 8
    RET

kernel_probe_rodata_store:
    ADDI32 SP, -8
    MOVI64 R0, kernel_write_protect_probe
    MOVI32U R1, 0x41
kernel_store_probe_instruction:
    STORE8 R0, R1
kernel_store_probe_resume:
    LOAD8U R1, R0
    CMPI32 R1, 0x5A
    BRCC NE, kernel_probe_failed
    MOVI32U R0, 0
    ADDI32 SP, 8
    RET

kernel_probe_data_execute:
    ADDI32 SP, -8
    MOVI64 R0, kernel_execution_protect_probe
kernel_execute_probe_instruction:
    CALLR R0
kernel_execute_probe_resume:
    MOVI32U R0, 0
    ADDI32 SP, 8
    RET

kernel_probe_failed:
    MOVI32U R0, 1
    ADDI32 SP, 8
    RET

.section .rodata
.global kernel_write_protect_probe
kernel_write_protect_probe:
    .byte 0x5A

.section .data
.align 8
.global kernel_execution_protect_probe
kernel_execution_protect_probe:
    .qword 0
