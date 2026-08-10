.section .text
.global add_values
.extern demo_result

add_values:
    ADD R0, R1
    MOVI64 R2, demo_result
    STORE64 R2, R0
    RET
