; Minimal executable Boot ROM image.
; The host maps this image read-only at 0x7FFFF00000 and resets PC there.

.entry boot

boot:
    MOVI64 R0, 0xFFFFFFFFFFFD0018
    MOVI32U R1, 1
    STORE64 R0, R1

    MOVI64 R0, 0xFFFFFFFFFFFD0000
    MOVI32U R1, 82
    STORE8 R0, R1
    MOVI32U R1, 79
    STORE8 R0, R1
    MOVI32U R1, 77
    STORE8 R0, R1
    MOVI32U R1, 10
    STORE8 R0, R1
    HALT
