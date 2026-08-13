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
.global kernel_device_irq_entry
.global kernel_ipi_entry
.global kernel_idle_wait
.global kernel_idle_wait_instruction
.global kernel_idle_wait_resume
.global kernel_suspend_to_user
.global kernel_switch_continuation
.global kernel_resume_continuation
.global kernel_start_user
.global kernel_secondary_trampoline
.global kernel_secondary_trampoline_end
.global kernel_secondary_table_load
.global kernel_secondary_restart
.global kernel_secondary_retire_entry
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
.extern kernel_devices_interrupt
.extern kernel_smp_trap_enter
.extern kernel_smp_trap_leave
.extern kernel_smp_ipi_interrupt
.extern kernel_smp_secondary_retire
.extern kernel_smp_retire_finalize
.extern kernel_secondary_main
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

; Secondary hardware threads start with MMU off, SP=0 and no register
; arguments. This page is temporarily identity-mapped by kernel/smp.c.
; The MOVI64 operand is patched once with the physical bootstrap table
; address before any secondary is started.
kernel_secondary_trampoline:
    COREID R0
    THREADID R1
kernel_secondary_table_load:
    MOVI64 R2, 0
    MOV R5, R2
    LOAD64O R3, R5, 0           ; threads_per_core
    MUL R0, R3
    ADD R0, R1                  ; logical processor index
    MOVI32U R3, 88
    MUL R0, R3                  ; sizeof(KernelCpuLocal) = 88
    ADD R2, R0
    ADDI32 R2, 40               ; offsetof(KernelSmpBootstrap, cpus)
    LOAD64O SP, R2, 0           ; aligned bootstrap stack top
    LOAD64O R4, R5, 16          ; physical PTBR
    LOAD64O R6, R5, 24          ; physical VBR
    LOAD64O R7, R5, 32          ; virtual kernel_secondary_main
    SETPTBR R4
    SETVBR R6
    SETKSP SP
    MMUON
    CALLR R7
    HALT
kernel_secondary_trampoline_end:

; Abandon a completed secondary user's kernel stack and resume the permanent
; bootstrap/idle context. This never returns to the retired trap frame.
kernel_secondary_restart:
    MOV SP, R0
    CALLREL kernel_smp_retire_finalize
    JUMPREL kernel_secondary_main
    HALT

; Target of an IRET-modified syscall/exception frame. IRET first clears the
; architectural exception-active state, then this entry abandons the retired
; thread stack through kernel_smp_secondary_retire.
kernel_secondary_retire_entry:
    CALLREL kernel_smp_secondary_retire
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

    CALLREL kernel_smp_trap_enter
    MOV R0, SP
    CALLREL kernel_exception_dispatch
    CMPI32 R0, 0
    BRCC NE, kernel_page_fault_fatal
    CALLREL kernel_smp_trap_leave

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
    CALLREL kernel_smp_trap_enter
    MOV R0, SP
    CALLREL kernel_syscall_dispatch
    CALLREL kernel_smp_trap_leave
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
    CALLREL kernel_smp_trap_enter
    MOV R0, SP
    CALLREL kernel_scheduler_timer
    CALLREL kernel_smp_trap_leave

    JUMPREL kernel_trap_restore

kernel_device_irq_entry:
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
    CALLREL kernel_smp_trap_enter
    MOV R0, SP
    CALLREL kernel_devices_interrupt
    CALLREL kernel_smp_trap_leave

    JUMPREL kernel_trap_restore

kernel_ipi_entry:
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
    CALLREL kernel_smp_trap_enter
    MOV R0, SP
    CALLREL kernel_smp_ipi_interrupt
    CALLREL kernel_smp_trap_leave

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

; Enter the hardware WAIT state without losing an IRQ delivered immediately
; after EI. The timer handler redirects a frame whose return PC still points
; at kernel_idle_wait_instruction to kernel_idle_wait_resume.
kernel_idle_wait:
    EI
kernel_idle_wait_instruction:
    WAIT
kernel_idle_wait_resume:
    DI
    RET

; Preserve a blocking syscall's complete C stack, then enter a thread that
; currently has only a saved user context. A later resume restores the saved
; SP and RETs to the instruction after this call.
kernel_suspend_to_user:
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
    STORE64 R0, SP
    SETPTBR R1
    SETKSP R4
    MOV SP, R4
    PUSH R3
    MOVI64 R14, 0x4000000000000010
    PUSH R14
    PUSH R2
    MOV R11, R5
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

; Both sides have a suspended kernel continuation.
kernel_switch_continuation:
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
    STORE64 R0, SP
    MOV SP, R1
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
    RET

; Enter a suspended kernel continuation from a user trap scheduler path.
kernel_resume_continuation:
    MOV SP, R0
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
    RET

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
