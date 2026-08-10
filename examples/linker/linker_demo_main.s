.include "linker_demo.inc"

.section .text
.global demo_entry
.extern add_values
.entry demo_entry

demo_entry:
    LOAD_PAIR DEMO_LEFT, DEMO_RIGHT
    CALLREL add_values
    HALT

.section .rodata
demo_name:
    .asciz "linked-demo"

.section .data
.global demo_result
demo_result:
    .qword 0

.section .bss
scratch:
    .space 64
