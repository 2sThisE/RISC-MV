; CVM integer/branch throughput benchmark
; Same benchmark as the first test.
; 100,000,000 iterations x (ADDI32 + JNZ)
; Total guest instructions: 200,000,002

MOVI32U R0, 100000000

loop:
    ADDI32 R0, -1
    JNZ loop

HALT
