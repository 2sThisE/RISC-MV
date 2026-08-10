; CVM integer/branch throughput benchmark
; Direct-load binary: main.exe -r 4096 -l bench_loop.bin
;
; Executed guest instructions:
;   1 x MOVI32U
;   100,000,000 x ADDI32
;   100,000,000 x JNZ
;   1 x HALT
; = 200,000,002 instructions total

MOVI32U R0, 100000000

loop:
    ADDI32 R0, -1
    JNZ loop

HALT
