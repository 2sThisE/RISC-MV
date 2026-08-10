; Minimal guest-kernel syscall demonstration.
; SYS_WRITE writes one byte to UART, SYS_EXIT halts in supervisor mode.

.entry boot

boot:
    MOVI32U SP, 0x700
    MOVI32U R10, 0x800
    SETKSP R10

    MOVI32U R11, 0x900
    SETVBR R11

    ; Enable UART before entering user mode.
    MOVI64 R12, 0xFFFFFFFFFFFD0018
    MOVI32U R13, 1
    STORE64 R12, R13

    ENTERUSER

user_program:
    MOVI32U R0, 1       ; SYS_WRITE
    MOVI32U R1, 83      ; ASCII 'S'
    SYSCALL

    MOVI32U R0, 0       ; SYS_EXIT
    SYSCALL
    NOP                 ; SYS_EXIT never returns

.org 0x200
syscall_handler:
    CMPI32 R0, 1
    BRCC EQ, sys_write
    CMPI32 R0, 0
    BRCC EQ, sys_exit

    MOVI32S R0, -1      ; unknown syscall
    IRET

sys_write:
    MOVI64 R2, 0xFFFFFFFFFFFD0000
    STORE8 R2, R1
    MOVI32U R0, 1
    IRET

sys_exit:
    HALT

; VBR=0x900, syscall vector index=76, entry size=8.
.org 0xB60
    .qword syscall_handler
