; CVM second-stage bootloader.
; Entry handoff:
;   R0 = CvmFirmwareTable at 0x6000
;   R1 = "CVMFWH01" handoff magic
;   R2 = boot-device slot

.entry loader_entry

loader_entry:
    DI
    MOVI32U SP, 0x3F000
    MOVI32U R3, 0x9000
    STORE64O R3, R0, 0x00
    MOV R10, R0
    MOVI64 R4, 0x31304857464D5643
    CMP R1, R4
    BRCC NE, loader_fatal_table
    LOAD64 R4, R10
    MOVI64 R5, 0x31303057464D5643
    CMP R4, R5
    BRCC NE, loader_fatal_table
    LOAD32UO R4, R10, 0x08
    CMPI32 R4, 1
    BRCC NE, loader_fatal_table
    LOAD32UO R4, R10, 0x0C
    CMPI32 R4, 256
    BRCC NE, loader_fatal_table
    LOAD32UO R4, R10, 0x10
    CMPI32 R4, 256
    BRCC NE, loader_fatal_table

    ; Verify the firmware table CRC32.
    LOAD32UO R4, R10, 0x18
    STORE64O R3, R4, 0x40
    MOVI32U R5, 0
    STORE32O R10, R5, 0x18
    MOV R0, R10
    MOVI32U R1, 256
    CALLREL crc32
    MOVI32U R3, 0x9000
    LOAD64O R4, R3, 0x40
    MOVI32U R10, 0x6000
    STORE32O R10, R4, 0x18
    CMP R0, R4
    BRCC NE, loader_fatal_table

    MOVI64 R0, loader_message_start
    MOVI32U R1, 18
    CALLREL firmware_console
    CMPI32 R0, 0
    BRCC NE, loader_fatal_halt

    ; Ask firmware for KERNEL.CVM size, then read it to staging RAM.
    MOVI64 R0, kernel_name
    LOAD64O R11, R10, 0xB0
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_file
    MOVI32U R3, 0x9000
    STORE64O R3, R1, 0x08
    CMPI32 R1, 192
    BRCC LTU, loader_fatal_file
    MOV R4, R1
    ADDI32 R4, 0x40000
    LOAD64O R5, R10, 0x20
    CMP R4, R5
    BRCC GTU, loader_fatal_file
    MOVI64 R0, kernel_name
    MOVI32U R1, 0x40000
    LOAD64O R2, R3, 0x08
    LOAD64O R11, R10, 0xB8
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_file

    CALLREL validate_kernel_image
    CMPI32 R0, 0
    BRCC NE, loader_fatal_kernel
    CALLREL load_kernel_image
    CMPI32 R0, 0
    BRCC NE, loader_fatal_kernel

    ; Obtain the final firmware map key after all file operations.
    MOVI32U R0, 0x8400
    MOVI32U R1, 2
    LOAD64O R11, R10, 0xA8
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_map
    MOVI32U R3, 0x9000
    STORE64O R3, R2, 0x28

    CALLREL build_boot_info
    CMPI32 R0, 0
    BRCC NE, loader_fatal_map

    MOVI64 R0, loader_message_kernel
    MOVI32U R1, 19
    CALLREL firmware_console
    CMPI32 R0, 0
    BRCC NE, loader_fatal_halt

    MOVI32U R3, 0x9000
    LOAD64O R0, R3, 0x28
    LOAD64O R11, R10, 0xC0
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_halt

    ; Services are now unavailable. Jump with the kernel ABI register state.
    MOVI32U R3, 0x9000
    LOAD64O R4, R3, 0x10
    MOVI32U R5, 0x7000
    MOVI32U R6, 5
    STORE8 R5, R6
    STORE64O R5, R4, 1
    MOVI32U R0, 0x8000
    MOVI64 R1, 0x31544F4F424D5643
    MOVI32U R2, 0
    MOVI32U R3, 0
    MOVI32U R4, 0
    MOVI32U R5, 0
    MOVI32U R6, 0
    MOVI32U R7, 0
    MOVI32U R8, 0
    MOVI32U R9, 0
    MOVI32U R10, 0
    MOVI32U R11, 0
    MOVI32U R12, 0
    MOVI32U R13, 0
    MOVI32U R14, 0
    MOVI32U SP, 0x3F000
    JUMP 0x7000

; Calls firmware ConsoleWrite while preserving the table in R10.
firmware_console:
    LOAD64O R11, R10, 0xA0
    CALLR R11
    RET

; ---------------------------------------------------------------------------
; KERNEL.CVM v1 multi-segment validation

validate_kernel_image:
    MOVI32U R3, 0x40000
    LOAD64 R4, R3
    MOVI64 R5, 0x314E52454B4D5643
    CMP R4, R5
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x08
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x0C
    CMPI32 R4, 128
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x10
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x18
    MOVI32U R5, 0x314D5643
    CMP R4, R5
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x1C
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    LOAD8UO R4, R3, 0x20
    CMPI32 R4, 64
    BRCC NE, validate_kernel_fail
    LOAD8UO R4, R3, 0x21
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    LOAD16UO R4, R3, 0x22
    CMPI32 R4, 0
    BRCC EQ, validate_kernel_fail
    CMPI32 R4, 64
    BRCC GTU, validate_kernel_fail
    MOV R14, R4
    LOAD32UO R4, R3, 0x24
    CMPI32 R4, 64
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x28
    CMPI32 R4, 128
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x38
    MOVI32U R6, 0x9000
    LOAD64O R5, R6, 0x08
    CMP R4, R5
    BRCC NE, validate_kernel_fail
    MOV R5, R14
    SHL R5, 6
    ADDI32 R5, 128
    LOAD64O R4, R6, 0x08
    CMP R5, R4
    BRCC GTU, validate_kernel_fail
    STORE64O R6, R5, 0x48
    LOAD64O R4, R3, 0x40
    MOVI32U R10, 0x6000
    LOAD64O R5, R10, 0x28
    MOV R7, R5
    NOT R7
    AND R4, R7
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail

    ; Header CRC.
    LOAD32UO R4, R3, 0x58
    STORE64O R6, R4, 0x40
    MOVI32U R5, 0
    STORE32O R3, R5, 0x58
    MOV R0, R3
    MOVI32U R1, 128
    CALLREL crc32
    MOVI32U R6, 0x9000
    LOAD64O R4, R6, 0x40
    MOVI32U R3, 0x40000
    STORE32O R3, R4, 0x58
    CMP R0, R4
    BRCC NE, validate_kernel_fail

    ; Payload CRC.
    MOVI32U R0, 0x40080
    LOAD64O R1, R6, 0x08
    ADDI32 R1, -128
    CALLREL crc32
    MOVI32U R3, 0x40000
    LOAD32UO R4, R3, 0x5C
    CMP R0, R4
    BRCC NE, validate_kernel_fail

    ; Validate sorted, non-overlapping load segments.
    MOVI32U R9, 0x9000
    MOVI32U R4, 0
    STORE64O R9, R4, 0x18
    STORE64O R9, R4, 0x20
    STORE64O R9, R4, 0x50
    MOVI32U R13, 0x40080
    MOV R12, R14
validate_segment_loop:
    MOV R3, R13
    LOAD32UO R4, R3, 0x00
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x04
    TESTI32 R4, 1
    BRCC EQ, validate_kernel_fail
    ANDI32 R4, 0xFFFFFFF8
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail
    LOAD64O R5, R3, 0x08
    LOAD64O R11, R9, 0x48
    CMP R5, R11
    BRCC LTU, validate_kernel_fail
    LOAD64O R6, R3, 0x20
    LOAD64O R7, R3, 0x28
    CMP R6, R7
    BRCC GTU, validate_kernel_fail
    MOV R8, R5
    ADD R8, R6
    CMP R8, R5
    BRCC LTU, validate_kernel_fail
    LOAD64O R4, R9, 0x08
    CMP R8, R4
    BRCC GTU, validate_kernel_fail
    LOAD64O R5, R3, 0x10
    CMPI32 R5, 0x10000
    BRCC LTU, validate_kernel_fail
    MOV R8, R5
    ADD R8, R7
    CMP R8, R5
    BRCC LTU, validate_kernel_fail
    MOVI32U R10, 0x6000
    LOAD64O R4, R10, 0x20
    CMP R8, R4
    BRCC GEU, validate_kernel_fail
    CMPI32 R8, 0x20000
    BRCC GTU, validate_kernel_fail
    LOAD64O R4, R9, 0x20
    CMP R5, R4
    BRCC LTU, validate_kernel_fail
    CMPI32 R4, 0
    BRCC NE, validate_have_minimum
    STORE64O R9, R5, 0x18
validate_have_minimum:
    LOAD64O R4, R3, 0x30
    CMPI32 R4, 0
    BRCC EQ, validate_kernel_fail
    MOV R11, R4
    ADDI32 R11, -1
    MOV R14, R4
    AND R14, R11
    CMPI32 R14, 0
    BRCC NE, validate_kernel_fail
    MOV R14, R5
    AND R14, R11
    CMPI32 R14, 0
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x38
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail
    STORE64O R9, R8, 0x20
    LOAD32UO R4, R3, 0x04
    TESTI32 R4, 4
    BRCC EQ, validate_segment_not_entry
    MOVI32U R11, 0x40000
    LOAD64O R4, R11, 0x30
    CMP R4, R5
    BRCC LTU, validate_segment_not_entry
    CMP R4, R8
    BRCC GEU, validate_segment_not_entry
    STORE64O R9, R4, 0x10
    MOVI32U R4, 1
    STORE64O R9, R4, 0x50
validate_segment_not_entry:
    ADDI32 R13, 64
    ADDI32 R12, -1
    BRCC NE, validate_segment_loop
    LOAD64O R4, R9, 0x50
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    MOVI32U R0, 0
    RET
validate_kernel_fail:
    MOVI32U R0, 1
    RET

load_kernel_image:
    MOVI32U R3, 0x40000
    LOAD16UO R12, R3, 0x22
    MOVI32U R13, 0x40080
load_segment_loop:
    LOAD64O R0, R13, 0x08
    ADDI32 R0, 0x40000
    LOAD64O R1, R13, 0x10
    LOAD64O R2, R13, 0x20
    CALLREL copy_bytes
    LOAD64O R0, R13, 0x10
    LOAD64O R1, R13, 0x20
    ADD R0, R1
    LOAD64O R2, R13, 0x28
    SUB R2, R1
    MOV R1, R2
    CALLREL zero_bytes
    ADDI32 R13, 64
    ADDI32 R12, -1
    BRCC NE, load_segment_loop
    MOVI32U R0, 0
    RET

; ---------------------------------------------------------------------------
; Kernel BootInfo

build_boot_info:
    MOVI32U R0, 0x8000
    MOVI32U R1, 416
    CALLREL zero_bytes
    MOVI32U R3, 0x8000
    MOVI64 R4, 0x31544F4F424D5643
    STORE64 R3, R4
    MOVI32U R4, 1
    STORE32O R3, R4, 0x08
    MOVI32U R4, 256
    STORE32O R3, R4, 0x0C
    MOVI32U R4, 416
    STORE32O R3, R4, 0x10
    MOVI32U R4, 16
    STORE32O R3, R4, 0x14
    MOVI32U R4, 256
    STORE64O R3, R4, 0x20
    MOVI32U R4, 5
    STORE32O R3, R4, 0x28
    MOVI32U R4, 32
    STORE32O R3, R4, 0x2C
    MOVI32U R10, 0x6000
    LOAD64O R4, R10, 0x20
    STORE64O R3, R4, 0x38
    MOVI32U R5, 0x9000
    LOAD64O R4, R5, 0x10
    STORE64O R3, R4, 0x40
    LOAD64O R4, R5, 0x18
    STORE64O R3, R4, 0x48
    LOAD64O R6, R5, 0x20
    SUB R6, R4
    STORE64O R3, R6, 0x50
    LOAD64O R4, R10, 0x48
    STORE64O R3, R4, 0x78
    LOAD64O R4, R10, 0x50
    STORE64O R3, R4, 0x80
    LOAD64O R4, R10, 0x58
    STORE64O R3, R4, 0x88
    LOAD64O R4, R10, 0x60
    STORE64O R3, R4, 0x90
    LOAD64O R4, R10, 0x68
    STORE64O R3, R4, 0x98
    LOAD64O R4, R10, 0x70
    STORE64O R3, R4, 0xA0
    LOAD64O R4, R10, 0x78
    STORE64O R3, R4, 0xA8
    LOAD32UO R4, R10, 0x80
    STORE32O R3, R4, 0xB0
    LOAD32UO R4, R10, 0x84
    STORE32O R3, R4, 0xB4
    LOAD64O R4, R10, 0x88
    STORE64O R3, R4, 0xB8
    LOAD64O R4, R10, 0x90
    STORE64O R3, R4, 0xC0
    LOAD64O R4, R10, 0x30
    STORE32O R3, R4, 0xC8
    LOAD64O R4, R10, 0x38
    STORE16O R3, R4, 0xCC
    LOAD64O R4, R10, 0x40
    STORE16O R3, R4, 0xCE

    MOVI32U R6, 0x8100
    MOVI32U R7, 0
    STORE64O R6, R7, 0x00
    MOVI32U R7, 0x8000
    STORE64O R6, R7, 0x08
    MOVI32U R7, 4
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    MOVI32U R7, 0x8000
    STORE64O R6, R7, 0x00
    MOVI32U R7, 0x200
    STORE64O R6, R7, 0x08
    MOVI32U R7, 5
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    MOVI32U R7, 0x8200
    STORE64O R6, R7, 0x00
    MOVI32U R5, 0x9000
    LOAD64O R8, R5, 0x18
    SUB R8, R7
    CMPI32 R8, 0
    BRCC EQ, build_boot_info_fail
    STORE64O R6, R8, 0x08
    MOVI32U R7, 4
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    LOAD64O R7, R5, 0x18
    STORE64O R6, R7, 0x00
    LOAD64O R8, R5, 0x20
    SUB R8, R7
    STORE64O R6, R8, 0x08
    MOVI32U R7, 3
    STORE32O R6, R7, 0x10
    MOVI32U R7, 7
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    LOAD64O R7, R5, 0x20
    STORE64O R6, R7, 0x00
    MOVI32U R10, 0x6000
    LOAD64O R8, R10, 0x20
    SUB R8, R7
    CMPI32 R8, 0
    BRCC EQ, build_boot_info_fail
    STORE64O R6, R8, 0x08
    MOVI32U R7, 1
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14
    MOVI32U R0, 0x8000
    MOVI32U R1, 416
    CALLREL crc32
    MOVI32U R3, 0x8000
    STORE32O R3, R0, 0xF0
    MOVI32U R0, 0
    RET
build_boot_info_fail:
    MOVI32U R0, 1
    RET

; ---------------------------------------------------------------------------
; Runtime helpers and fatal reporting

copy_bytes:
    CMPI32 R2, 0
    BRCC EQ, copy_done
copy_loop:
    LOAD8U R3, R0
    STORE8 R1, R3
    ADDI32 R0, 1
    ADDI32 R1, 1
    ADDI32 R2, -1
    BRCC NE, copy_loop
copy_done:
    RET

zero_bytes:
    MOVI32U R2, 0
    CMPI32 R1, 0
    BRCC EQ, zero_done
zero_loop:
    STORE8 R0, R2
    ADDI32 R0, 1
    ADDI32 R1, -1
    BRCC NE, zero_loop
zero_done:
    RET

crc32:
    MOVI64 R2, 0xFFFFFFFF
crc_byte:
    CMPI32 R1, 0
    BRCC EQ, crc_done
    LOAD8U R3, R0
    XOR R2, R3
    MOVI32U R4, 8
crc_bit:
    MOV R5, R2
    ANDI32 R5, 1
    SHR R2, 1
    CMPI32 R5, 0
    BRCC EQ, crc_next_bit
    XORI32 R2, 0xEDB88320
crc_next_bit:
    ADDI32 R4, -1
    BRCC NE, crc_bit
    ADDI32 R0, 1
    ADDI32 R1, -1
    JUMPREL crc_byte
crc_done:
    NOT R2
    ZEXT32 R0, R2
    RET

loader_fatal_table:
    MOVI64 R0, loader_message_table
    MOVI32U R1, 25
    JUMPREL loader_fatal_print
loader_fatal_file:
    MOVI64 R0, loader_message_file
    MOVI32U R1, 27
    JUMPREL loader_fatal_print
loader_fatal_kernel:
    MOVI64 R0, loader_message_bad_kernel
    MOVI32U R1, 31
    JUMPREL loader_fatal_print
loader_fatal_map:
    MOVI64 R0, loader_message_map
    MOVI32U R1, 27
loader_fatal_print:
    MOVI32U R10, 0x6000
    CALLREL firmware_console
loader_fatal_halt:
    HALT

kernel_name:
    .ascii "KERNEL  CVM"
loader_message_start:
    .ascii "CVM LOADER: start\n"
loader_message_kernel:
    .ascii "CVM LOADER: kernel\n"
loader_message_table:
    .ascii "CVM LOADER E01: firmware\n"
loader_message_file:
    .ascii "CVM LOADER E02: KERNEL.CVM\n"
loader_message_bad_kernel:
    .ascii "CVM LOADER E03: invalid kernel\n"
loader_message_map:
    .ascii "CVM LOADER E04: memory map\n"
