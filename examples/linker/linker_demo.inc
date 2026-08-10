.equ DEMO_LEFT, 4 * 5
.equ DEMO_RIGHT, (1 << 5) | 10

.macro LOAD_PAIR left, right
    MOVI32U R0, \left
    MOVI32U R1, \right
.endm
