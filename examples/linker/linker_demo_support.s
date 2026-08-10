.section .text
.global add_values
.extern demo_result
.type add_values, function
.type demo_result, object

add_values:
    ADD R0, R1
    MOVI64 R2, demo_result
    STORE64 R2, R0
    RET
.size add_values, $ - add_values

.section .data
.global override_value
.type override_value, object
override_value:
    .qword 0x1122334455667788
.size override_value, $ - override_value

.section .bss
.comm scratch, 128, 32
