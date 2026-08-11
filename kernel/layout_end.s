.section .text
.global kernel_text_end
kernel_text_end:

.section .rodata
.global kernel_rodata_end
kernel_rodata_end:

.section .data
.global kernel_data_end
kernel_data_end:

.section .bss
.align 16
.global kernel_stack_bottom
.global kernel_stack_top
kernel_stack_bottom:
    ; The reference bootstrap stack is part of the kernel memory span. The
    ; loader checks that this end remains below its RAM-top staging buffer.
    .space 0x6000
kernel_stack_top:
.global kernel_bss_end
kernel_bss_end:
