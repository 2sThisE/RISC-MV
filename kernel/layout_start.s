.section .text
.global kernel_text_start
kernel_text_start:
    NOP

.section .rodata
.global kernel_rodata_start
kernel_rodata_start:

.section .data
.global kernel_data_start
kernel_data_start:

.section .bss
.global kernel_bss_start
kernel_bss_start:
