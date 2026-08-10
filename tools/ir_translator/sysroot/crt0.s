.section .text
.global _start
.extern main
.type _start, function
.type main, function
.entry _start

_start:
    MOVI64 SP, __cvm_default_stack_top
    CALLREL main
    HALT
.size _start, $ - _start

.section .bss
.align 16
__cvm_default_stack:
    .space 65536
__cvm_default_stack_top:
