; Attach block_device.dll as the first external module.
; Writes a 0x5A-filled sector to LBA 1, reads it back, then prints B.

.entry start

start:
    MOVI64 R0, 0xFFFFFFFFF0000000 ; first dynamic BAR

    MOVI32U R7, 1
    STORE64O R0, R7, 0x30         ; LBA = 1
    MOVI32U R7, 0x800
    STORE64O R0, R7, 0x38         ; DMA source
    MOVI32U R7, 1
    STORE64O R0, R7, 0x40         ; one sector
    MOVI32U R7, 2
    STORE64O R0, R7, 0x48         ; WRITE

wait_write:
    LOAD64O R1, R0, 0x50
    TESTI32 R1, 0x0C              ; DONE or ERROR
    BRCC EQ, wait_write
    TESTI32 R1, 0x08
    BRCC NE, failed

    MOVI32U R7, 3
    STORE64O R0, R7, 0x48         ; FLUSH

wait_flush:
    LOAD64O R1, R0, 0x50
    TESTI32 R1, 0x0C
    BRCC EQ, wait_flush
    TESTI32 R1, 0x08
    BRCC NE, failed

    MOVI32U R7, 0xA00
    STORE64O R0, R7, 0x38         ; DMA destination
    MOVI32U R7, 1
    STORE64O R0, R7, 0x48         ; READ

wait_read:
    LOAD64O R1, R0, 0x50
    TESTI32 R1, 0x0C
    BRCC EQ, wait_read
    TESTI32 R1, 0x08
    BRCC NE, failed

    MOVI32U R2, 0xA00
    LOAD8U R3, R2
    CMPI32 R3, 0x5A
    BRCC NE, failed

    MOVI32U R5, 66                ; ASCII 'B'
    JUMPREL print_result

failed:
    MOVI32U R5, 69                ; ASCII 'E'

print_result:
    MOVI64 R4, 0xFFFFFFFFFFFD0018 ; UART CONTROL
    MOVI32U R6, 1
    STORE64 R4, R6
    MOVI64 R4, 0xFFFFFFFFFFFD0000 ; UART TXDATA
    STORE8 R4, R5
    HALT

.org 0x800
write_buffer:
    .space 512, 0x5A

.org 0xA00
read_buffer:
    .space 512, 0
