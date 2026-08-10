.section .text
.global cvm_kernel_memory_entry
.extern cvm_kernel_memory_demo
.type cvm_kernel_memory_entry, function
.type cvm_kernel_memory_demo, function
.entry cvm_kernel_memory_entry

cvm_kernel_memory_entry:
    MOVI64 SP, 0x100000
    CALLREL cvm_kernel_memory_demo
    HALT
.size cvm_kernel_memory_entry, $ - cvm_kernel_memory_entry
