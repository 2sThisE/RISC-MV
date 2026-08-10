; CVM RAM LOAD64/STORE64 throughput benchmark with MMU ON.
;
; RAM layout used by this benchmark:
;   physical 0x00000 page : code + scratch data, identity-mapped VA 0x00000
;   physical 0x10000 page : L2 root page table
;   physical 0x11000 page : L1 page table
;   physical 0x12000 page : L0 page table
;
; For VA 0x00000000, L2/L1/L0 index are all zero.
; PTE flags for the leaf are:
;   VALID(1) | READ(2) | WRITE(4) | EXECUTE(8) = 0x0F
;
; Current CVM docs say there is no TLB, so this benchmark intentionally
; exercises page-table walking for instruction fetches and data accesses.
;
; Loop body is identical to bench_memory.asm.
; Total guest instructions including setup: 180,000,017

; Build a 3-level identity mapping for virtual page 0 -> physical page 0.
MOVI64  R8, 0x0000000000010000
MOVI64  R9, 0x0000000000011001
STORE64 R8, R9

MOVI64  R8, 0x0000000000011000
MOVI64  R9, 0x0000000000012001
STORE64 R8, R9

MOVI64  R8, 0x0000000000012000
MOVI64  R9, 0x000000000000000F
STORE64 R8, R9

MOVI64  R8, 0x0000000000010000
SETPTBR R8
MMUON

; From here, instruction fetch and ordinary memory addresses are virtual.
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
