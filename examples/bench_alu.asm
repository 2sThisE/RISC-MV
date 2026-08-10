; CVM ALU throughput benchmark
; 10,000,000 iterations.
; Each loop: 16 ALU ops + counter decrement + conditional branch = 18 instructions.
; Approx total guest instructions: 180,000,003

MOVI32U R0, 10000000
MOVI32U R1, 0

loop:
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R1, 1
    ADDI32 R0, -1
    JNZ loop

HALT
