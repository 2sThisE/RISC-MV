; CVM RAM LOAD64/STORE64 throughput benchmark (MMU OFF)
; 10,000,000 iterations.
; Each loop: 8 LOAD64 + 8 STORE64 + decrement + branch = 18 instructions.
; Total guest instructions: 180,000,005
;
; Direct-load binaries start at RAM address 1. 0x800 is kept as a small
; scratch location in the same first 4 KiB RAM page.

MOVI32U R0, 10000000
MOVI32U R1, 0x800
MOVI64  R2, 0x1122334455667788
STORE64 R1, R2

loop:
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2
    LOAD64  R2, R1
    STORE64 R1, R2

    ADDI32 R0, -1
    JNZ loop

HALT
