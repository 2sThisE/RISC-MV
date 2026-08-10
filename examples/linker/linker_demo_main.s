.include "linker_demo.inc"

.section .text
.global demo_entry
.extern add_values
.weak optional_hook
.type demo_entry, function
.type add_values, function
.type optional_hook, function
.entry demo_entry

demo_entry:
    LOAD_PAIR DEMO_LEFT, DEMO_RIGHT
    CALLREL add_values
    MOVI64 R3, optional_hook
    HALT
.size demo_entry, $ - demo_entry

.section .rodata
demo_name:
    .asciz "linked-demo"

.section .data
.global demo_result
.type demo_result, object
demo_result:
    .qword 0
.size demo_result, $ - demo_result

.section .bss
.comm scratch, 64, 16
.comm override_value, 8, 8
