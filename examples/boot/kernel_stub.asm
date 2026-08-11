; Minimal kernel entry that validates the Boot ROM handoff and BootInfo CRC32.

.entry kernel_entry

kernel_entry:
    MOVI32U SP, 0x40000
    MOVI32U R10, 0x8000
    CMP R0, R10
    BRCC NE, boot_failed
    MOVI64 R10, 0x31544F4F424D5643
    CMP R1, R10
    BRCC NE, boot_failed
    LOAD64 R10, R0
    CMP R10, R1
    BRCC NE, boot_failed
    LOAD32UO R10, R0, 0x10
    CMPI32 R10, 416
    BRCC NE, boot_failed
    LOAD32UO R11, R0, 0xF0
    MOVI32U R12, 0
    STORE32O R0, R12, 0xF0
    MOV R12, R0
    MOV R13, R10
    CALLREL crc32
    MOVI32U R12, 0x8000
    STORE32O R12, R11, 0xF0
    CMP R0, R11
    BRCC NE, boot_failed

    MOVI64 R0, message_ok
    CALLREL uart_puts
    MOVI32U R3, 42
    HALT

boot_failed:
    MOVI64 R0, message_failed
    CALLREL uart_puts
    HALT

crc32:
    MOVI64 R2, 0xFFFFFFFF
crc32_byte_loop:
    CMPI32 R13, 0
    BRCC EQ, crc32_done
    LOAD8U R3, R12
    XOR R2, R3
    MOVI32U R4, 8
crc32_bit_loop:
    MOV R5, R2
    ANDI32 R5, 1
    SHR R2, 1
    CMPI32 R5, 0
    BRCC EQ, crc32_bit_next
    XORI32 R2, 0xEDB88320
crc32_bit_next:
    ADDI32 R4, -1
    BRCC NE, crc32_bit_loop
    ADDI32 R12, 1
    ADDI32 R13, -1
    JUMPREL crc32_byte_loop
crc32_done:
    NOT R2
    ZEXT32 R0, R2
    RET

uart_puts:
    MOVI64 R2, 0xFFFFFFFFFFFD0000
uart_puts_loop:
    LOAD8U R1, R0
    CMPI32 R1, 0
    BRCC EQ, uart_puts_done
    STORE8 R2, R1
    ADDI32 R0, 1
    JUMPREL uart_puts_loop
uart_puts_done:
    RET

message_ok:
    .asciz "KERNEL: BootInfo OK\n"
message_failed:
    .asciz "KERNEL: BootInfo ERROR\n"
