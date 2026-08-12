; First RArchM64 user process. The kernel enters with the startup ABI:
; R0=argc, R1=argv, R2=envp, SP=initial user stack.

.entry init_entry

init_entry:
    ; Additional threads start at the same entry with R4=0 and run the
    ; scheduler worker path. PID 1 receives child PID/TID/status in R4-R6.
    CMPI32 R0, 2
    BRCC EQ, init_execed
    CMPI32 R4, 0
    BRCC EQ, init_worker
    MOV R9, R4
    MOV R10, R5
    MOV R11, R6

    MOVI32U R13, 65
    CMPI32 R0, 1
    BRCC NE, init_failed
    MOVI32U R13, 66
    LOAD64 R4, R1
    LOAD8U R5, R4
    CMPI32 R5, 47
    BRCC NE, init_failed

    MOV R2, SP
    ADDI32 R2, -8
    MOVI32U R13, 67
    MOVI32U R0, 6
    MOV R1, R9
    SYSCALL
    CMP R0, R9
    BRCC NE, init_failed
    MOVI32U R13, 68
    LOAD64 R7, R2
    CMP R7, R11
    BRCC NE, init_failed

    MOVI32U R13, 69
    MOVI32U R0, 7
    MOV R1, R10
    SYSCALL
    CMP R0, R10
    BRCC NE, init_failed
    MOVI32U R13, 70
    LOAD64 R7, R2
    CMPI32 R7, 0
    BRCC NE, init_failed

    MOVI32U R13, 71
    MOVI32U R0, 8
    MOVI64 R1, init_test_path
    MOVI32U R2, 3
    SYSCALL
    CMPI32 R0, 3
    BRCC NE, init_failed
    MOV R12, R0

    MOVI32U R13, 72
    MOVI32U R0, 10
    MOV R1, R12
    MOVI32U R2, 4
    MOVI32U R3, 0
    SYSCALL
    CMPI32 R0, 4
    BRCC NE, init_failed

    MOVI32U R13, 73
    MOV R2, SP
    ADDI32 R2, -16
    MOVI32U R0, 2
    MOV R1, R12
    MOVI32U R3, 2
    SYSCALL
    CMPI32 R0, 2
    BRCC NE, init_failed
    MOVI32U R13, 74
    LOAD8U R7, R2
    CMPI32 R7, 45
    BRCC NE, init_failed
    MOVI32U R13, 75
    LOAD8UO R7, R2, 1
    CMPI32 R7, 77
    BRCC NE, init_failed

    MOVI32U R13, 76
    MOVI32U R0, 10
    MOV R1, R12
    MOVI32U R2, 4
    MOVI32U R3, 0
    SYSCALL
    CMPI32 R0, 4
    BRCC NE, init_failed
    MOVI32U R13, 77
    MOVI32U R0, 1
    MOV R1, R12
    MOV R2, SP
    ADDI32 R2, -16
    MOVI32U R3, 2
    SYSCALL
    CMPI32 R0, 2
    BRCC NE, init_failed

    MOVI32U R13, 78
    MOVI32U R0, 11
    MOV R1, R12
    SYSCALL
    CMPI32 R0, 0
    BRCC NE, init_failed

    MOVI32U R13, 79
    MOVI32U R0, 9
    MOV R1, R12
    SYSCALL
    CMPI32 R0, 0
    BRCC NE, init_failed

    MOVI32U R13, 80
    MOVI32U R0, 9
    MOV R1, R12
    SYSCALL
    CMPI32 R0, -9
    BRCC NE, init_failed

    MOVI32U R13, 81
    MOVI32U R0, 1
    MOVI32U R1, 1
    MOVI32U R2, 0
    MOVI32U R3, 1
    SYSCALL
    CMPI32 R0, -14
    BRCC NE, init_failed

    MOVI32U R13, 82
    MOVI32U R0, 8
    MOVI64 R1, init_test_path
    MOVI32U R2, 1
    SYSCALL
    CMPI32 R0, 3
    BRCC NE, init_failed

    MOVI32U R13, 83
    MOVI32U R0, 12
    MOVI64 R1, init_missing_path
    MOVI32U R2, 0
    MOVI32U R3, 0
    SYSCALL
    CMPI32 R0, -2
    BRCC NE, init_failed

    MOVI32U R13, 84
    MOVI32U R0, 12
    MOVI64 R1, init_test_path
    MOVI32U R2, 0
    MOVI32U R3, 0
    SYSCALL
    CMPI32 R0, -8
    BRCC NE, init_failed

    MOVI32U R13, 85
    MOVI32U R0, 12
    MOVI64 R1, init_exec_path
    MOVI32U R2, 1
    MOVI32U R3, 0
    SYSCALL
    CMPI32 R0, -14
    BRCC NE, init_failed

    MOVI32U R13, 86
    MOV R2, SP
    ADDI32 R2, -64
    MOVI64 R7, init_exec_path
    STORE64O R2, R7, 0
    MOVI64 R7, init_exec_argument
    STORE64O R2, R7, 8
    MOVI32U R7, 0
    STORE64O R2, R7, 16
    MOV R3, SP
    ADDI32 R3, -32
    MOVI64 R7, init_exec_environment
    STORE64O R3, R7, 0
    MOVI32U R7, 0
    STORE64O R3, R7, 8
    MOVI32U R0, 12
    MOVI64 R1, init_exec_path
    SYSCALL
    BRCC ALWAYS, init_failed

init_execed:
    MOVI32U R13, 87
    CMPI32 R0, 2
    BRCC NE, init_failed
    MOVI32U R13, 88
    LOAD64O R4, R1, 8
    LOAD8U R5, R4
    CMPI32 R5, 45
    BRCC NE, init_failed
    LOAD64 R4, R2
    LOAD8U R5, R4
    CMPI32 R5, 69
    BRCC NE, init_failed

    MOVI32U R13, 89
    MOVI32U R0, 10
    MOVI32U R1, 3
    MOVI32U R2, 4
    MOVI32U R3, 0
    SYSCALL
    CMPI32 R0, 4
    BRCC NE, init_failed
    MOVI32U R13, 90
    MOV R2, SP
    ADDI32 R2, -16
    MOVI32U R0, 2
    MOVI32U R1, 3
    MOVI32U R3, 2
    SYSCALL
    CMPI32 R0, 2
    BRCC NE, init_failed
    LOAD8U R7, R2
    CMPI32 R7, 45
    BRCC NE, init_failed
    LOAD8UO R7, R2, 1
    CMPI32 R7, 77
    BRCC NE, init_failed
    MOVI32U R0, 9
    MOVI32U R1, 3
    SYSCALL
    CMPI32 R0, 0
    BRCC NE, init_failed

    MOVI32U R0, 1
    MOVI32U R1, 1
    MOVI64 R2, init_exec_message
    MOVI32U R3, 17
    SYSCALL
    MOVI32U R0, 0
    MOVI32U R1, 0
    SYSCALL
    HALT

init_worker:
    MOVI32U R0, 3
    SYSCALL
    MOVI32U R8, 500000
init_worker_loop:
    ADDI32 R8, -1
    CMPI32 R8, 0
    BRCC NE, init_worker_loop
    MOVI32U R0, 0
    MOVI32U R1, 0
    SYSCALL
    HALT

init_failed:
    MOVI32U R0, 1
    MOVI32U R1, 1
    MOVI64 R2, init_error
    MOVI32U R3, 12
    SYSCALL
    MOV R2, SP
    ADDI32 R2, -24
    STORE8 R2, R13
    MOVI32U R7, 10
    STORE8O R2, R7, 1
    MOVI32U R0, 1
    MOVI32U R1, 1
    MOVI32U R3, 2
    SYSCALL
    MOVI32U R0, 0
    MOVI32U R1, 1
    SYSCALL
    HALT

init_exec_message:
    .ascii "INIT: EXEC/FD OK\n"
init_error:
    .ascii "INIT: ERROR "
init_test_path:
    .ascii "/KTEST.TXT"
    .byte 0
init_missing_path:
    .ascii "/NO.EXF"
    .byte 0
init_exec_path:
    .ascii "/BIN/INIT.EXF"
    .byte 0
init_exec_argument:
    .ascii "--execed"
    .byte 0
init_exec_environment:
    .ascii "EXEC=1"
    .byte 0
