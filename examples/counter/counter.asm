; vmasm example: count down, call a function, then halt.
; Assemble at address 1 because main.exe -l loads raw binaries there.

.entry start

start:
    MOVI32U SP, 4096
    MOVI32U R0, 5

countdown:
    ADDI32 R0, -1
    CMPI32 R0, 0
    BRCC NE, countdown

    CALLREL set_result
    HALT

set_result:
    MOVI32U R1, 42
    RET

.align 8
message:
    .asciz "counter complete"
