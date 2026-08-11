; RISC-VM Boot ROM v1.
;
; Reset address: 0x0000007FFFF00000
; Required RAM: 1 MiB or more
; Disk path: VIO storage -> GPT -> RISC-VM Boot partition -> FAT32
; Second-stage path: /BOOT/BOOT.EXF
;
; Low RAM firmware layout:
;   0x1000..0x11FF  private state
;   0x2000..0x5FFF  sector/GPT scratch
;   0x6000..0x60FF  CvmFirmwareTable
;   0x7000..0x7008  absolute-jump trampoline
;   0xF000          downward-growing firmware stack top
;   0x18000..0x181FF read-only FAT sector cache
;   0x40000..       first-stage BOOT.EXF staging (reused after handoff)

.entry boot

boot:
    DI
    MOVI32U SP, 0xF000

    ; Enable the built-in UART.
    MOVI64 R0, 0xFFFFFFFFFFFD0018
    MOVI32U R1, 1
    STORE64 R0, R1
    MOVI64 R0, message_start
    CALLREL uart_puts

    ; Record host-provided hardware information.
    MOVI64 R0, 0xFFFFFFFFFFFC1000
    LOAD64O R1, R0, 0x20
    MOVI32U R2, 0x1000
    STORE64O R2, R1, 0x10
    MOVI32U R3, 0x100000
    CMP R1, R3
    BRCC LTU, error_ram
    LOAD64O R1, R0, 0x10
    STORE64O R2, R1, 0x80
    LOAD64O R1, R0, 0x58
    STORE64O R2, R1, 0x70
    LOAD64O R1, R0, 0x60
    STORE64O R2, R1, 0x88
    LOAD64O R1, R0, 0x68
    STORE64O R2, R1, 0x78

    CALLREL find_storage
    CMPI32 R0, 0
    BRCC EQ, error_device

    CALLREL load_gpt
    CMPI32 R0, 0
    BRCC EQ, error_gpt

    CALLREL load_fat_metadata
    CMPI32 R0, 0
    BRCC EQ, error_fat

    ; Locate BOOT in the FAT32 root directory.
    MOVI32U R0, 0x1000
    LOAD64O R0, R0, 0x40
    CALLREL read_cluster
    CMPI32 R0, 0
    BRCC EQ, error_io
    MOVI32U R0, 0x2000
    MOVI32U R3, 0x1000
    LOAD64O R1, R3, 0x28
    SHL R1, 9
    MOVI32U R2, 0
    CALLREL find_directory_entry
    CMPI32 R0, 0
    BRCC EQ, error_bootdir
    MOVI32U R3, 0x1000
    STORE64O R3, R0, 0x100

    ; Locate BOOT.EXF in BOOT.
    CALLREL read_cluster
    CMPI32 R0, 0
    BRCC EQ, error_io
    MOVI32U R0, 0x2000
    MOVI32U R3, 0x1000
    LOAD64O R1, R3, 0x28
    SHL R1, 9
    MOVI32U R2, 1
    CALLREL find_directory_entry
    CMPI32 R0, 0
    BRCC EQ, error_kernel_file
    MOVI32U R3, 0x1000
    STORE64O R3, R0, 0x48
    STORE64O R3, R1, 0x50

    CALLREL stage_kernel_file
    CMPI32 R0, 0
    BRCC EQ, error_chain

    CALLREL validate_kernel
    CMPI32 R0, 0
    BRCC EQ, error_kernel

    CALLREL load_kernel_segments
    CMPI32 R0, 0
    BRCC EQ, error_kernel

    CALLREL build_firmware_table
    CMPI32 R0, 0
    BRCC EQ, error_bootinfo

    MOVI64 R0, message_handoff
    CALLREL uart_puts

    ; Build a RAM trampoline so every unspecified GPR can be zero at handoff.
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x58
    MOVI32U R5, 0x7000
    MOVI32U R6, 5                 ; OP_JUMP
    STORE8 R5, R6
    STORE64O R5, R4, 1

    MOVI32U R0, 0x6000
    MOVI64 R1, 0x31304857464D5643 ; "CVMFWH01"
    MOVI32U R3, 0x1000
    LOAD64O R2, R3, 0x08
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
    MOVI32U SP, 0xF000
    JUMP 0x7000

; ---------------------------------------------------------------------------
; VIO and block I/O

; Returns R0=1 and stores BAR/slot on success, otherwise R0=0.
find_storage:
    MOVI64 R3, 0xFFFFFFFFFFFC0000
    LOAD64O R4, R3, 0x00
    MOVI32U R5, 0x314F4956
    CMP R4, R5
    BRCC NE, find_storage_fail
    LOAD64O R4, R3, 0x10
    MOVI32U R5, 0
    LEA R6, R3, 0x100
find_storage_loop:
    CMP R5, R4
    BRCC GEU, find_storage_fail
    LOAD64O R7, R6, 0x00
    TESTI32 R7, 1
    BRCC EQ, find_storage_next
    LOAD64O R7, R6, 0x08
    CMPI32 R7, 6
    BRCC NE, find_storage_next
    LOAD64O R7, R6, 0x28
    LOAD64O R8, R7, 0x00
    MOVI64 R9, 0x314B4C42564D
    CMP R8, R9
    BRCC NE, find_storage_next
    LOAD64O R8, R7, 0x20
    CMPI32 R8, 512
    BRCC NE, find_storage_next
    MOVI32U R9, 0x1000
    STORE64O R9, R7, 0x00
    STORE64O R9, R5, 0x08
    MOVI32U R0, 1
    RET
find_storage_next:
    ADDI32 R5, 1
    ADDI32 R6, 0xC0
    JUMPREL find_storage_loop
find_storage_fail:
    MOVI32U R0, 0
    RET

; R0=LBA, R1=DMA address, R2=sector count. Returns R0=1/0.
block_read:
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x00
    STORE64O R4, R0, 0x30
    STORE64O R4, R1, 0x38
    STORE64O R4, R2, 0x40
    MOVI32U R5, 1
    STORE64O R4, R5, 0x48
block_read_wait:
    LOAD64O R5, R4, 0x50
    TESTI32 R5, 0x0C
    BRCC EQ, block_read_wait
    TESTI32 R5, 0x08
    BRCC NE, block_read_fail
    MOVI32U R0, 1
    RET
block_read_fail:
    MOVI32U R0, 0
    RET

; ---------------------------------------------------------------------------
; GPT

load_gpt:
    MOVI32U R0, 1
    MOVI32U R1, 0x2000
    MOVI32U R2, 1
    CALLREL block_read
    CMPI32 R0, 0
    BRCC EQ, load_gpt_fail

    MOVI32U R3, 0x2000
    LOAD64 R4, R3
    MOVI64 R5, 0x5452415020494645 ; "EFI PART"
    CMP R4, R5
    BRCC NE, load_gpt_fail
    LOAD32UO R4, R3, 0x0C
    CMPI32 R4, 92
    BRCC LTU, load_gpt_fail
    CMPI32 R4, 512
    BRCC GTU, load_gpt_fail
    MOVI32U R6, 0x1000
    STORE64O R6, R4, 0xA8
    LOAD32UO R5, R3, 0x10
    STORE64O R6, R5, 0xA0
    MOVI32U R7, 0
    STORE32O R3, R7, 0x10
    MOV R0, R3
    MOV R1, R4
    CALLREL crc32
    LOAD64O R5, R6, 0xA0
    CMP R0, R5
    BRCC NE, load_gpt_header_crc_fail

    MOVI32U R3, 0x2000
    LOAD64O R7, R3, 0x48
    LOAD32UO R8, R3, 0x50
    LOAD32UO R9, R3, 0x54
    CMPI32 R8, 128
    BRCC NE, load_gpt_fail
    CMPI32 R9, 128
    BRCC NE, load_gpt_fail
    LOAD32UO R10, R3, 0x58
    STORE64O R6, R10, 0xA0

    MOV R0, R7
    MOVI32U R1, 0x2000
    MOVI32U R2, 32
    CALLREL block_read
    CMPI32 R0, 0
    BRCC EQ, load_gpt_fail
    MOVI32U R0, 0x2000
    MOVI32U R1, 0x4000
    CALLREL crc32
    MOVI32U R6, 0x1000
    LOAD64O R10, R6, 0xA0
    CMP R0, R10
    BRCC NE, load_gpt_entries_crc_fail

    MOVI32U R3, 0x2000
    MOVI32U R4, 0
load_gpt_entry_loop:
    CMPI32 R4, 128
    BRCC GEU, load_gpt_fail
    LOAD64 R5, R3
    MOVI64 R6, 0x4BC86D529F7C3A21
    CMP R5, R6
    BRCC NE, load_gpt_entry_next
    LOAD64O R5, R3, 8
    MOVI64 R6, 0x4F4F424D5643E1A3
    CMP R5, R6
    BRCC NE, load_gpt_entry_next
    LOAD64O R5, R3, 32
    LOAD64O R6, R3, 40
    CMP R6, R5
    BRCC LTU, load_gpt_fail
    MOV R7, R6
    SUB R7, R5
    ADDI32 R7, 1
    MOVI32U R8, 0x1000
    STORE64O R8, R5, 0x18
    STORE64O R8, R7, 0x20
    ADDI32 R4, 1
    STORE64O R8, R4, 0x90
    MOVI32U R0, 1
    RET
load_gpt_entry_next:
    ADDI32 R3, 128
    ADDI32 R4, 1
    JUMPREL load_gpt_entry_loop
load_gpt_fail:
    MOVI32U R0, 0
    RET
load_gpt_header_crc_fail:
    MOVI64 R0, message_gpt_header_crc
    CALLREL uart_puts
    HALT
load_gpt_entries_crc_fail:
    MOVI64 R0, message_gpt_entries_crc
    CALLREL uart_puts
    HALT

; ---------------------------------------------------------------------------
; FAT32

load_fat_metadata:
    MOVI32U R3, 0x1000
    LOAD64O R0, R3, 0x18
    MOVI32U R1, 0x2000
    MOVI32U R2, 1
    CALLREL block_read
    CMPI32 R0, 0
    BRCC EQ, load_fat_fail
    MOVI32U R4, 0x2000
    LOAD16UO R5, R4, 11
    CMPI32 R5, 512
    BRCC NE, load_fat_fail
    LOAD8UO R5, R4, 13
    CMPI32 R5, 0
    BRCC EQ, load_fat_fail
    MOV R6, R5
    ADDI32 R6, -1
    AND R6, R5
    CMPI32 R6, 0
    BRCC NE, load_fat_fail
    LOAD16UO R6, R4, 14
    CMPI32 R6, 0
    BRCC EQ, load_fat_fail
    LOAD8UO R7, R4, 16
    CMPI32 R7, 0
    BRCC EQ, load_fat_fail
    LOAD32UO R8, R4, 36
    CMPI32 R8, 0
    BRCC EQ, load_fat_fail
    LOAD32UO R9, R4, 44
    CMPI32 R9, 2
    BRCC LTU, load_fat_fail
    LOAD16UO R10, R4, 510
    CMPI32 R10, 0xAA55
    BRCC NE, load_fat_fail
    LOAD32UO R10, R4, 32
    LOAD64O R11, R3, 0x20
    CMP R10, R11
    BRCC NE, load_fat_fail

    STORE64O R3, R5, 0x28
    STORE64O R3, R9, 0x40
    LOAD64O R10, R3, 0x18
    ADD R10, R6
    STORE64O R3, R10, 0x30
    MOV R11, R8
    MUL R11, R7
    ADD R10, R11
    STORE64O R3, R10, 0x38
    STORE64O R3, R8, 0x98
    MOVI32U R10, 0
    STORE64O R3, R10, 0x158      ; FAT cache invalid
    MOVI32U R0, 1
    RET
load_fat_fail:
    MOVI32U R0, 0
    RET

; R0=cluster. Reads the complete cluster to 0x2000.
read_cluster:
    CMPI32 R0, 2
    BRCC LTU, read_cluster_fail
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x28
    MOV R5, R0
    ADDI32 R5, -2
    MUL R5, R4
    LOAD64O R0, R3, 0x38
    ADD R0, R5
    MOVI32U R1, 0x2000
    MOV R2, R4
    CALLREL block_read
    RET
read_cluster_fail:
    MOVI32U R0, 0
    RET

; R0=directory base, R1=size, R2=0 for BOOT dir, 1 for BOOT.EXF,
; or 2 for KERNEL.EXF.
; Returns R0=cluster, R1=file size. R0=0 means not found.
find_directory_entry:
    MOVI32U R3, 0
find_directory_loop:
    CMP R3, R1
    BRCC GEU, find_directory_fail
    MOV R4, R0
    ADD R4, R3
    LOAD8U R5, R4
    CMPI32 R5, 0
    BRCC EQ, find_directory_fail
    CMPI32 R5, 0xE5
    BRCC EQ, find_directory_next
    LOAD8UO R5, R4, 11
    CMPI32 R5, 0x0F
    BRCC EQ, find_directory_next
    CMPI32 R2, 0
    BRCC NE, find_directory_file
    LOAD64 R6, R4
    MOVI64 R7, 0x20202020544F4F42
    CMP R6, R7
    BRCC NE, find_directory_next
    LOAD32UO R6, R4, 8
    ANDI32 R6, 0x00FFFFFF
    CMPI32 R6, 0x00202020
    BRCC NE, find_directory_next
    TESTI32 R5, 0x10
    BRCC EQ, find_directory_next
    JUMPREL find_directory_match
find_directory_file:
    CMPI32 R2, 1
    BRCC NE, find_directory_kernel
    LOAD64 R6, R4
    MOVI64 R7, 0x20202020544F4F42 ; "BOOT    "
    CMP R6, R7
    BRCC NE, find_directory_next
    LOAD32UO R6, R4, 8
    ANDI32 R6, 0x00FFFFFF
    CMPI32 R6, 0x00465845          ; "EXF"
    BRCC NE, find_directory_next
    TESTI32 R5, 0x10
    BRCC NE, find_directory_next
    JUMPREL find_directory_match
find_directory_kernel:
    LOAD64 R6, R4
    MOVI64 R7, 0x20204C454E52454B
    CMP R6, R7
    BRCC NE, find_directory_next
    LOAD32UO R6, R4, 8
    ANDI32 R6, 0x00FFFFFF
    CMPI32 R6, 0x00465845
    BRCC NE, find_directory_next
    TESTI32 R5, 0x10
    BRCC NE, find_directory_next
find_directory_match:
    LOAD16UO R6, R4, 20
    SHL R6, 16
    LOAD16UO R7, R4, 26
    OR R6, R7
    CMPI32 R6, 2
    BRCC LTU, find_directory_fail
    LOAD32UO R1, R4, 28
    MOV R0, R6
    RET
find_directory_next:
    ADDI32 R3, 32
    JUMPREL find_directory_loop
find_directory_fail:
    MOVI32U R0, 0
    MOVI32U R1, 0
    RET

; R0=cluster, returns masked next FAT32 cluster or zero on I/O failure.
fat_next:
    MOVI32U R3, 0x1000
    MOV R4, R0
    SHL R4, 2
    MOV R5, R4
    SHR R5, 9
    ANDI32 R4, 0x1FF
    STORE64O R3, R4, 0xB0
    LOAD64O R0, R3, 0x30
    ADD R0, R5
    LOAD64O R6, R3, 0x158
    CMPI32 R6, 1
    BRCC NE, fat_next_cache_miss
    LOAD64O R6, R3, 0x160
    CMP R0, R6
    BRCC EQ, fat_next_cached
fat_next_cache_miss:
    STORE64O R3, R0, 0x160
    MOVI32U R1, 0x18000
    MOVI32U R2, 1
    CALLREL block_read
    CMPI32 R0, 0
    BRCC EQ, fat_next_fail
    MOVI32U R3, 0x1000
    MOVI32U R6, 1
    STORE64O R3, R6, 0x158
fat_next_cached:
    LOAD64O R4, R3, 0xB0
    MOVI32U R5, 0x18000
    ADD R5, R4
    LOAD32U R0, R5
    ANDI32 R0, 0x0FFFFFFF
    RET
fat_next_fail:
    MOVI32U R0, 0
    RET

stage_kernel_file:
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x48        ; current cluster
    LOAD64O R5, R3, 0x50        ; remaining file bytes
    CMPI32 R5, 0
    BRCC EQ, stage_kernel_fail
    LOAD64O R6, R3, 0x28
    SHL R6, 9                    ; cluster bytes
    MOVI32U R7, 0x40000          ; staging cursor
stage_kernel_loop:
    MOV R8, R7
    ADD R8, R6
    LOAD64O R9, R3, 0x10
    CMP R8, R9
    BRCC GTU, stage_kernel_ram_fail
    STORE64O R3, R4, 0xB0
    STORE64O R3, R5, 0xB8
    STORE64O R3, R6, 0xC0
    STORE64O R3, R7, 0xC8
    LOAD64O R10, R3, 0x28
    MOV R11, R4
    ADDI32 R11, -2
    MUL R11, R10
    LOAD64O R0, R3, 0x38
    ADD R0, R11
    MOV R1, R7
    MOV R2, R10
    CALLREL block_read
    CMPI32 R0, 0
    BRCC EQ, stage_kernel_io_fail
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0xB0
    MOV R0, R4
    CALLREL fat_next
    CMPI32 R0, 0
    BRCC EQ, stage_kernel_next_fail
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0xB0
    LOAD64O R5, R3, 0xB8
    LOAD64O R6, R3, 0xC0
    LOAD64O R7, R3, 0xC8
    CMP R5, R6
    BRCC LEU, stage_kernel_last
    CMPI32 R0, 2
    BRCC LTU, stage_kernel_fail
    MOVI32U R9, 0x0FFFFFF8
    CMP R0, R9
    BRCC GEU, stage_kernel_early_eoc_fail
    SUB R5, R6
    ADD R7, R6
    MOV R4, R0
    JUMPREL stage_kernel_loop
stage_kernel_last:
    MOVI32U R9, 0x0FFFFFF8
    CMP R0, R9
    BRCC LTU, stage_kernel_last_eoc_fail
    MOVI32U R0, 1
    RET
stage_kernel_fail:
    MOVI32U R0, 0
    RET
stage_kernel_ram_fail:
    MOVI64 R0, message_chain_ram
    CALLREL uart_puts
    HALT
stage_kernel_io_fail:
    MOVI64 R0, message_chain_io
    CALLREL uart_puts
    HALT
stage_kernel_next_fail:
    MOVI64 R0, message_chain_next
    CALLREL uart_puts
    HALT
stage_kernel_early_eoc_fail:
    MOVI64 R0, message_chain_early
    CALLREL uart_puts
    HALT
stage_kernel_last_eoc_fail:
    MOVI64 R0, message_chain_last
    CALLREL uart_puts
    HALT

; ---------------------------------------------------------------------------
; RISC-VM EXF validation and loading

validate_kernel:
    MOVI32U R3, 0x40000
    LOAD64 R4, R3
    MOVI64 R5, 0x31304658454D5652 ; "RVMEXF01"
    CMP R4, R5
    BRCC NE, validate_kernel_fail
    LOAD16UO R4, R3, 0x08
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    LOAD16UO R4, R3, 0x0A
    CMPI32 R4, 0
    BRCC GTU, validate_kernel_fail
    LOAD32UO R4, R3, 0x0C
    CMPI32 R4, 128
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x10
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x18
    MOVI32U R5, 0x314D5652        ; "RVM1"
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
    CMPI32 R4, 1
    BRCC LTU, validate_kernel_fail
    CMPI32 R4, 64
    BRCC GTU, validate_kernel_fail
    MOVI32U R6, 0x1000
    STORE64O R6, R4, 0xD0
    LOAD32UO R5, R3, 0x24
    CMPI32 R5, 64
    BRCC NE, validate_kernel_fail
    LOAD64O R5, R3, 0x28
    CMPI32 R5, 128
    BRCC LTU, validate_kernel_fail
    STORE64O R6, R5, 0xD8
    LOAD64O R7, R3, 0x38
    LOAD64O R8, R6, 0x50
    CMP R7, R8
    BRCC NE, validate_kernel_fail
    LOAD64O R7, R3, 0x30
    STORE64O R6, R7, 0x58
    LOAD64O R7, R3, 0x40
    LOAD64O R8, R6, 0x80
    MOV R9, R8
    NOT R9
    AND R7, R9
    CMPI32 R7, 0
    BRCC NE, validate_kernel_fail

    ; Header CRC with the stored field treated as zero.
    LOAD32UO R7, R3, 0x58
    STORE64O R6, R7, 0xA0
    MOVI32U R8, 0
    STORE32O R3, R8, 0x58
    MOV R0, R3
    MOVI32U R1, 128
    CALLREL crc32
    MOVI32U R6, 0x1000
    LOAD64O R7, R6, 0xA0
    CMP R0, R7
    BRCC NE, validate_kernel_fail

    ; Payload CRC.
    MOVI32U R3, 0x40080
    LOAD64O R1, R6, 0x50
    ADDI32 R1, -128
    MOV R0, R3
    CALLREL crc32
    MOVI32U R3, 0x40000
    LOAD32UO R7, R3, 0x5C
    CMP R0, R7
    BRCC NE, validate_kernel_fail

    ; Validate table bounds and every segment before writing RAM.
    MOVI32U R6, 0x1000
    LOAD64O R4, R6, 0xD0
    SHL R4, 6
    LOAD64O R5, R6, 0xD8
    MOV R7, R5
    ADD R7, R4
    CMP R7, R5
    BRCC LTU, validate_kernel_fail
    LOAD64O R8, R6, 0x50
    CMP R7, R8
    BRCC GTU, validate_kernel_fail
    STORE64O R6, R7, 0xE0
    MOVI64 R7, 0xFFFFFFFFFFFFFFFF
    STORE64O R6, R7, 0x60
    MOVI32U R7, 0
    STORE64O R6, R7, 0x68
    STORE64O R6, R7, 0xE8       ; entry found
    STORE64O R6, R7, 0xF0       ; segment index

validate_segment_loop:
    LOAD64O R7, R6, 0xF0
    LOAD64O R8, R6, 0xD0
    CMP R7, R8
    BRCC GEU, validate_segments_done
    MOV R9, R7
    SHL R9, 6
    LOAD64O R10, R6, 0xD8
    ADD R9, R10
    ADDI32 R9, 0x40000
    LOAD32UO R10, R9, 0x00
    CMPI32 R10, 1
    BRCC NE, validate_kernel_fail
    LOAD32UO R10, R9, 0x04
    TESTI32 R10, 1
    BRCC EQ, validate_kernel_fail
    MOV R11, R10
    ANDI32 R11, 0xFFFFFFF8
    CMPI32 R11, 0
    BRCC NE, validate_kernel_fail
    LOAD64O R11, R9, 0x20       ; file size
    LOAD64O R12, R9, 0x28       ; memory size
    CMPI32 R12, 0
    BRCC EQ, validate_kernel_fail
    CMP R11, R12
    BRCC GTU, validate_kernel_fail
    LOAD64O R13, R9, 0x30       ; alignment
    CMPI32 R13, 0
    BRCC EQ, validate_kernel_fail
    MOV R14, R13
    ADDI32 R14, -1
    MOV R3, R13
    AND R3, R14
    CMPI32 R3, 0
    BRCC NE, validate_kernel_fail
    LOAD64O R3, R9, 0x10        ; load address
    MOV R4, R3
    AND R4, R14
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail
    CMPI32 R3, 0x10000
    BRCC LTU, validate_kernel_fail
    MOV R4, R3
    ADD R4, R12                 ; memory end
    CMP R4, R3
    BRCC LTU, validate_kernel_fail
    LOAD64O R5, R6, 0x10
    CMP R4, R5
    BRCC GEU, validate_kernel_fail

    ; Refuse overlap with the staged image rounded up to its last cluster.
    CMPI32 R3, 0x40000
    BRCC GEU, validate_target_after_staging
    CMPI32 R4, 0x40000
    BRCC GTU, validate_kernel_fail
    JUMPREL validate_target_ok
validate_target_after_staging:
    LOAD64O R5, R6, 0x50
    LOAD64O R13, R6, 0x28
    SHL R13, 9
    ADDI32 R13, -1
    ADD R5, R13
    NOT R13
    AND R5, R13
    ADDI32 R5, 0x40000
    CMP R3, R5
    BRCC LTU, validate_kernel_fail
validate_target_ok:
    LOAD64O R5, R9, 0x08        ; file offset
    LOAD64O R13, R6, 0xE0
    CMP R5, R13
    BRCC LTU, validate_kernel_fail
    MOV R13, R5
    ADD R13, R11
    CMP R13, R5
    BRCC LTU, validate_kernel_fail
    LOAD64O R14, R6, 0x50
    CMP R13, R14
    BRCC GTU, validate_kernel_fail
    LOAD64O R13, R9, 0x38
    CMPI32 R13, 0
    BRCC NE, validate_kernel_fail

    ; Detect segment overlap against all preceding entries.
    MOVI32U R13, 0
validate_overlap_loop:
    CMP R13, R7
    BRCC GEU, validate_overlap_done
    MOV R14, R13
    SHL R14, 6
    LOAD64O R5, R6, 0xD8
    ADD R14, R5
    ADDI32 R14, 0x40000
    LOAD64O R5, R14, 0x10
    LOAD64O R14, R14, 0x28
    ADD R14, R5
    CMP R3, R14
    BRCC GEU, validate_overlap_next
    CMP R5, R4
    BRCC LTU, validate_kernel_fail
validate_overlap_next:
    ADDI32 R13, 1
    JUMPREL validate_overlap_loop
validate_overlap_done:
    LOAD64O R5, R6, 0x60
    CMP R3, R5
    BRCC GEU, validate_min_done
    STORE64O R6, R3, 0x60
validate_min_done:
    LOAD64O R5, R6, 0x68
    CMP R4, R5
    BRCC LEU, validate_max_done
    STORE64O R6, R4, 0x68
validate_max_done:
    TESTI32 R10, 4
    BRCC EQ, validate_entry_done
    LOAD64O R5, R6, 0x58
    CMP R5, R3
    BRCC LTU, validate_entry_done
    CMP R5, R4
    BRCC GEU, validate_entry_done
    MOVI32U R5, 1
    STORE64O R6, R5, 0xE8
validate_entry_done:
    ADDI32 R7, 1
    STORE64O R6, R7, 0xF0
    JUMPREL validate_segment_loop

validate_segments_done:
    LOAD64O R7, R6, 0xE8
    CMPI32 R7, 1
    BRCC NE, validate_kernel_fail
    MOVI32U R0, 1
    RET
validate_kernel_fail:
    MOVI32U R0, 0
    RET

load_kernel_segments:
    MOVI32U R3, 0x1000
    MOVI32U R4, 0
load_segment_loop:
    LOAD64O R5, R3, 0xD0
    CMP R4, R5
    BRCC GEU, load_segments_done
    MOV R6, R4
    SHL R6, 6
    LOAD64O R7, R3, 0xD8
    ADD R6, R7
    ADDI32 R6, 0x40000
    STORE64O R3, R4, 0xF0
    STORE64O R3, R6, 0xF8
    LOAD64O R7, R6, 0x08
    ADDI32 R7, 0x40000
    LOAD64O R8, R6, 0x10
    LOAD64O R9, R6, 0x20
    MOV R0, R7
    MOV R1, R8
    MOV R2, R9
    CALLREL copy_bytes
    MOVI32U R3, 0x1000
    LOAD64O R6, R3, 0xF8
    LOAD64O R8, R6, 0x10
    LOAD64O R9, R6, 0x20
    ADD R8, R9
    LOAD64O R10, R6, 0x28
    SUB R10, R9
    MOV R0, R8
    MOV R1, R10
    CALLREL zero_bytes
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0xF0
    ADDI32 R4, 1
    JUMPREL load_segment_loop
load_segments_done:
    MOVI32U R0, 1
    RET

; ---------------------------------------------------------------------------
; CVM firmware table and boot services

build_firmware_table:
    MOVI32U R0, 0x6000
    MOVI32U R1, 256
    CALLREL zero_bytes
    MOVI32U R3, 0x6000
    MOVI64 R4, 0x31303057464D5643 ; "CVMFW001"
    STORE64 R3, R4
    MOVI32U R4, 1
    STORE32O R3, R4, 0x08       ; ABI 1.0
    MOVI32U R4, 256
    STORE32O R3, R4, 0x0C
    STORE32O R3, R4, 0x10
    MOVI32U R5, 0x1000
    LOAD64O R4, R5, 0x10
    STORE64O R3, R4, 0x20       ; RAM size
    LOAD64O R4, R5, 0x80
    STORE64O R3, R4, 0x28       ; CPU features
    LOAD64O R4, R5, 0x88
    STORE64O R3, R4, 0x30       ; page size
    LOAD64O R4, R5, 0x70
    STORE64O R3, R4, 0x38       ; physical bits
    LOAD64O R4, R5, 0x78
    STORE64O R3, R4, 0x40       ; virtual bits
    MOVI64 R4, 0xFFFFFFFFFFFC0000
    STORE64O R3, R4, 0x48
    MOVI64 R4, 0xFFFFFFFFFFFC1000
    STORE64O R3, R4, 0x50
    MOVI64 R4, 0xFFFFFFFFFFFC2000
    STORE64O R3, R4, 0x58
    MOVI64 R4, 0xFFFFFFFFFFFC3000
    STORE64O R3, R4, 0x60
    MOVI64 R4, 0xFFFFFFFFFFFE0000
    STORE64O R3, R4, 0x68
    MOVI64 R4, 0xFFFFFFFFFFFF0000
    STORE64O R3, R4, 0x70
    MOVI64 R4, 0xFFFFFFFFFFFD0000
    STORE64O R3, R4, 0x78
    LOAD64O R4, R5, 0x08
    STORE32O R3, R4, 0x80       ; boot slot
    LOAD64O R4, R5, 0x90
    STORE32O R3, R4, 0x84       ; GPT partition index
    LOAD64O R4, R5, 0x18
    STORE64O R3, R4, 0x88
    LOAD64O R4, R5, 0x20
    STORE64O R3, R4, 0x90
    MOVI32U R4, 1
    STORE64O R3, R4, 0x98       ; memory-map key
    STORE64O R5, R4, 0x108      ; services active
    STORE64O R5, R4, 0x110      ; current map key
    MOVI64 R4, firmware_console_write
    STORE64O R3, R4, 0xA0
    MOVI64 R4, firmware_get_memory_map
    STORE64O R3, R4, 0xA8
    MOVI64 R4, firmware_get_file_size
    STORE64O R3, R4, 0xB0
    MOVI64 R4, firmware_read_file
    STORE64O R3, R4, 0xB8
    MOVI64 R4, firmware_exit_boot_services
    STORE64O R3, R4, 0xC0
    MOVI64 R4, firmware_reset_system
    STORE64O R3, R4, 0xC8
    MOVI32U R0, 0x6000
    MOVI32U R1, 256
    CALLREL crc32
    MOVI32U R3, 0x6000
    STORE32O R3, R0, 0x18
    MOVI32U R0, 1
    RET

; R0=bytes, R1=length. Returns R0=0 on success.
firmware_console_write:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOV R3, R0
    MOV R4, R1
    MOVI32U R5, 0x1000
    LOAD64O R6, R5, 0x108
    CMPI32 R6, 1
    BRCC NE, firmware_console_inactive
    MOVI64 R5, 0xFFFFFFFFFFFD0000
firmware_console_loop:
    CMPI32 R4, 0
    BRCC EQ, firmware_console_ok
    LOAD8U R6, R3
    STORE8 R5, R6
    ADDI32 R3, 1
    ADDI32 R4, -1
    JUMPREL firmware_console_loop
firmware_console_ok:
    MOVI32U R0, 0
    JUMPREL firmware_service_restore
firmware_console_inactive:
    MOVI32U R0, 1
    JUMPREL firmware_service_restore

; R0=entry buffer, R1=capacity in entries. Returns status R0, count R1,
; current map key R2. The base map has firmware low RAM and remaining RAM.
firmware_get_memory_map:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x108
    CMPI32 R4, 1
    BRCC NE, firmware_map_fail
    CMPI32 R1, 2
    BRCC LTU, firmware_map_fail
    MOVI32U R4, 0
    STORE64O R0, R4, 0x00
    MOVI32U R4, 0x10000
    STORE64O R0, R4, 0x08
    MOVI32U R4, 7               ; FIRMWARE
    STORE32O R0, R4, 0x10
    MOVI32U R4, 3
    STORE32O R0, R4, 0x14
    ADDI32 R0, 32
    MOVI32U R4, 0x10000
    STORE64O R0, R4, 0x00
    LOAD64O R5, R3, 0x10
    SUB R5, R4
    STORE64O R0, R5, 0x08
    MOVI32U R4, 1               ; USABLE
    STORE32O R0, R4, 0x10
    MOVI32U R4, 3
    STORE32O R0, R4, 0x14
    LOAD64O R2, R3, 0x110
    MOVI32U R1, 2
    MOVI32U R0, 0
    JUMPREL firmware_service_restore
firmware_map_fail:
    MOVI32U R2, 0
    MOVI32U R1, 2               ; required entry count
    MOVI32U R0, 1
    JUMPREL firmware_service_restore

; Finds an exact 11-byte FAT short name.
; R0=directory, R1=size, R2=name. Returns cluster R0 and size R1.
find_named_file:
    MOVI32U R3, 0
find_named_file_entry:
    CMP R3, R1
    BRCC GEU, find_named_file_fail
    MOV R4, R0
    ADD R4, R3
    LOAD8U R5, R4
    CMPI32 R5, 0
    BRCC EQ, find_named_file_fail
    CMPI32 R5, 0xE5
    BRCC EQ, find_named_file_next
    LOAD8UO R5, R4, 11
    CMPI32 R5, 0x0F
    BRCC EQ, find_named_file_next
    TESTI32 R5, 0x10
    BRCC NE, find_named_file_next
    MOVI32U R6, 0
find_named_file_compare:
    CMPI32 R6, 11
    BRCC GEU, find_named_file_match
    MOV R7, R4
    ADD R7, R6
    LOAD8U R8, R7
    MOV R7, R2
    ADD R7, R6
    LOAD8U R9, R7
    CMP R8, R9
    BRCC NE, find_named_file_next
    ADDI32 R6, 1
    JUMPREL find_named_file_compare
find_named_file_match:
    LOAD16UO R6, R4, 20
    SHL R6, 16
    LOAD16UO R7, R4, 26
    OR R6, R7
    CMPI32 R6, 2
    BRCC LTU, find_named_file_fail
    LOAD32UO R1, R4, 28
    MOV R0, R6
    RET
find_named_file_next:
    ADDI32 R3, 32
    JUMPREL find_named_file_entry
find_named_file_fail:
    MOVI32U R0, 0
    MOVI32U R1, 0
    RET

; R0=11-byte FAT name. Returns status R0 and file size R1.
firmware_get_file_size:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x108
    CMPI32 R4, 1
    BRCC NE, firmware_file_size_fail
    STORE64O R3, R0, 0x118
    LOAD64O R0, R3, 0x100
    CALLREL read_cluster
    CMPI32 R0, 0
    BRCC EQ, firmware_file_size_fail
    MOVI32U R3, 0x1000
    MOVI32U R0, 0x2000
    LOAD64O R1, R3, 0x28
    SHL R1, 9
    LOAD64O R2, R3, 0x118
    CALLREL find_named_file
    CMPI32 R0, 0
    BRCC EQ, firmware_file_size_fail
    MOVI32U R0, 0
    JUMPREL firmware_service_restore
firmware_file_size_fail:
    MOVI32U R1, 0
    MOVI32U R0, 1
    JUMPREL firmware_service_restore

; R0=11-byte FAT name, R1=destination, R2=capacity.
; Returns status R0 and exact file size R1.
firmware_read_file:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x108
    CMPI32 R4, 1
    BRCC NE, firmware_read_file_fail
    STORE64O R3, R0, 0x118
    STORE64O R3, R1, 0x120
    STORE64O R3, R2, 0x128
    LOAD64O R0, R3, 0x100
    CALLREL read_cluster
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail
    MOVI32U R3, 0x1000
    MOVI32U R0, 0x2000
    LOAD64O R1, R3, 0x28
    SHL R1, 9
    LOAD64O R2, R3, 0x118
    CALLREL find_named_file
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x128
    CMP R1, R4
    BRCC GTU, firmware_read_file_fail
    STORE64O R3, R0, 0x130
    STORE64O R3, R1, 0x138
    STORE64O R3, R1, 0x148
firmware_read_cluster_loop:
    MOVI32U R3, 0x1000
    LOAD64O R8, R3, 0x130       ; first cluster in this transfer
    LOAD64O R4, R3, 0x148       ; remaining file bytes
    LOAD64O R6, R3, 0x28        ; sectors per cluster
    MOV R7, R6
    SHL R7, 9                    ; bytes per cluster
    CMP R4, R7
    BRCC LEU, firmware_read_single_cluster

    ; Build the longest consecutive full-cluster run that fits in the block
    ; protocol's 128-sector transfer limit. A fragmented edge falls back to
    ; the next run without assuming that the whole file is contiguous.
    MOVI32U R11, 128
    DIVU R11, R6                 ; device-limited cluster count
    CMPI32 R11, 0
    BRCC EQ, firmware_read_single_cluster
    MOV R12, R4
    DIVU R12, R7                 ; complete clusters left in the file
    CMP R11, R12
    BRCC LEU, firmware_read_run_limit_ready
    MOV R11, R12
firmware_read_run_limit_ready:
    MOV R9, R8                   ; last cluster in run
    MOVI32U R10, 1               ; run cluster count
firmware_read_run_scan:
    CMP R10, R11
    BRCC GEU, firmware_read_run_scanned
    MOV R0, R9
    CALLREL fat_next
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail
    MOV R12, R9
    ADDI32 R12, 1
    CMP R0, R12
    BRCC NE, firmware_read_run_scanned
    MOV R9, R0
    ADDI32 R10, 1
    JUMPREL firmware_read_run_scan

firmware_read_run_scanned:
    ; Record the chain successor before block_read clobbers scratch GPRs.
    MOV R0, R9
    CALLREL fat_next
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail
    MOV R13, R0

    MOVI32U R3, 0x1000
    LOAD64O R6, R3, 0x28
    MOV R5, R8
    ADDI32 R5, -2
    MUL R5, R6
    LOAD64O R0, R3, 0x38
    ADD R0, R5                   ; first data LBA
    LOAD64O R1, R3, 0x120       ; direct destination DMA
    MOV R2, R10
    MUL R2, R6                   ; sector count
    STORE64O R3, R2, 0x168
    STORE64O R3, R10, 0x170
    STORE64O R3, R13, 0x178
    CALLREL block_read
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail

    MOVI32U R3, 0x1000
    LOAD64O R2, R3, 0x168
    SHL R2, 9                    ; transferred bytes
    LOAD64O R4, R3, 0x148
    SUB R4, R2
    STORE64O R3, R4, 0x148
    LOAD64O R5, R3, 0x120
    ADD R5, R2
    STORE64O R3, R5, 0x120
    LOAD64O R13, R3, 0x178
    CMPI32 R4, 0
    BRCC EQ, firmware_read_run_last
    CMPI32 R13, 2
    BRCC LTU, firmware_read_file_fail
    MOVI32U R5, 0x0FFFFFF8
    CMP R13, R5
    BRCC GEU, firmware_read_file_fail
    STORE64O R3, R13, 0x130
    JUMPREL firmware_read_cluster_loop

firmware_read_run_last:
    MOVI32U R5, 0x0FFFFFF8
    CMP R13, R5
    BRCC LTU, firmware_read_file_fail
    LOAD64O R1, R3, 0x138
    MOVI32U R0, 0
    JUMPREL firmware_service_restore

firmware_read_single_cluster:
    MOV R0, R8
    CALLREL read_cluster
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x148
    LOAD64O R5, R3, 0x28
    SHL R5, 9
    CMP R4, R5
    BRCC LEU, firmware_read_last_amount
    MOV R6, R5
    JUMPREL firmware_read_copy
firmware_read_last_amount:
    MOV R6, R4
firmware_read_copy:
    STORE64O R3, R6, 0x150
    MOVI32U R0, 0x2000
    LOAD64O R1, R3, 0x120
    MOV R2, R6
    CALLREL copy_bytes
    MOVI32U R3, 0x1000
    LOAD64O R6, R3, 0x150
    LOAD64O R4, R3, 0x148
    SUB R4, R6
    STORE64O R3, R4, 0x148
    LOAD64O R5, R3, 0x120
    ADD R5, R6
    STORE64O R3, R5, 0x120
    LOAD64O R0, R3, 0x130
    CALLREL fat_next
    CMPI32 R0, 0
    BRCC EQ, firmware_read_file_fail
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x148
    CMPI32 R4, 0
    BRCC EQ, firmware_read_file_last
    MOVI32U R5, 0x0FFFFFF8
    CMP R0, R5
    BRCC GEU, firmware_read_file_fail
    STORE64O R3, R0, 0x130
    JUMPREL firmware_read_cluster_loop
firmware_read_file_last:
    MOVI32U R5, 0x0FFFFFF8
    CMP R0, R5
    BRCC LTU, firmware_read_file_fail
    LOAD64O R1, R3, 0x138
    MOVI32U R0, 0
    JUMPREL firmware_service_restore
firmware_read_file_fail:
    MOVI32U R1, 0
    MOVI32U R0, 1
    JUMPREL firmware_service_restore

; R0=map key. Disables every boot service except ResetSystem.
firmware_exit_boot_services:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    MOVI32U R3, 0x1000
    LOAD64O R4, R3, 0x108
    CMPI32 R4, 1
    BRCC NE, firmware_exit_fail
    LOAD64O R4, R3, 0x110
    CMP R0, R4
    BRCC NE, firmware_exit_fail
    MOVI32U R4, 0
    STORE64O R3, R4, 0x108
    MOVI32U R0, 0
    JUMPREL firmware_service_restore
firmware_exit_fail:
    MOVI32U R0, 1
    JUMPREL firmware_service_restore

; R0=1 shutdown or 2 warm reset.
firmware_reset_system:
    PUSH R10
    PUSH R11
    PUSH R12
    PUSH R13
    PUSH R14
    CMPI32 R0, 1
    BRCC EQ, firmware_reset_send
    CMPI32 R0, 2
    BRCC NE, firmware_reset_fail
firmware_reset_send:
    MOVI64 R3, 0xFFFFFFFFFFFC2000
    STORE64O R3, R0, 0x18
    MOVI32U R0, 0
    JUMPREL firmware_service_restore
firmware_reset_fail:
    MOVI32U R0, 1

firmware_service_restore:
    POP R14
    POP R13
    POP R12
    POP R11
    POP R10
    RET

; ---------------------------------------------------------------------------
; BootInfo

build_boot_info:
    MOVI32U R0, 0x8000
    MOVI32U R1, 416
    CALLREL zero_bytes
    MOVI32U R3, 0x8000
    MOVI64 R4, 0x31544F4F424D5643 ; "CVMBOOT1"
    STORE64 R3, R4
    MOVI32U R4, 1
    STORE32O R3, R4, 0x08
    MOVI32U R4, 256
    STORE32O R3, R4, 0x0C
    MOVI32U R4, 416
    STORE32O R3, R4, 0x10
    MOVI32U R4, 16                ; GPT_BOOT
    STORE32O R3, R4, 0x14
    MOVI32U R4, 256
    STORE64O R3, R4, 0x20
    MOVI32U R4, 5
    STORE32O R3, R4, 0x28
    MOVI32U R4, 32
    STORE32O R3, R4, 0x2C
    MOVI32U R5, 0x1000
    LOAD64O R4, R5, 0x10
    STORE64O R3, R4, 0x38
    LOAD64O R4, R5, 0x58
    STORE64O R3, R4, 0x40
    LOAD64O R4, R5, 0x60
    STORE64O R3, R4, 0x48
    LOAD64O R6, R5, 0x68
    SUB R6, R4
    STORE64O R3, R6, 0x50
    MOVI64 R4, 0xFFFFFFFFFFFC0000
    STORE64O R3, R4, 0x78
    MOVI64 R4, 0xFFFFFFFFFFFC1000
    STORE64O R3, R4, 0x80
    MOVI64 R4, 0xFFFFFFFFFFFC2000
    STORE64O R3, R4, 0x88
    MOVI64 R4, 0xFFFFFFFFFFFC3000
    STORE64O R3, R4, 0x90
    MOVI64 R4, 0xFFFFFFFFFFFE0000
    STORE64O R3, R4, 0x98
    MOVI64 R4, 0xFFFFFFFFFFFF0000
    STORE64O R3, R4, 0xA0
    MOVI64 R4, 0xFFFFFFFFFFFD0000
    STORE64O R3, R4, 0xA8
    LOAD64O R4, R5, 0x08
    STORE32O R3, R4, 0xB0
    LOAD64O R4, R5, 0x90
    STORE32O R3, R4, 0xB4
    LOAD64O R4, R5, 0x18
    STORE64O R3, R4, 0xB8
    LOAD64O R4, R5, 0x20
    STORE64O R3, R4, 0xC0
    LOAD64O R4, R5, 0x88
    STORE32O R3, R4, 0xC8
    LOAD64O R4, R5, 0x70
    STORE16O R3, R4, 0xCC
    LOAD64O R4, R5, 0x78
    STORE16O R3, R4, 0xCE

    ; Five sorted, non-overlapping RAM map entries.
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
    LOAD64O R8, R5, 0x60
    SUB R8, R7
    CMPI32 R8, 0
    BRCC EQ, build_boot_info_fail
    STORE64O R6, R8, 0x08
    MOVI32U R7, 4
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14

    ADDI32 R6, 32
    LOAD64O R7, R5, 0x60
    STORE64O R6, R7, 0x00
    LOAD64O R8, R5, 0x68
    SUB R8, R7
    CMPI32 R8, 0
    BRCC EQ, build_boot_info_fail
    STORE64O R6, R8, 0x08
    MOVI32U R7, 3
    STORE32O R6, R7, 0x10
    MOVI32U R7, 7
    STORE32O R6, R7, 0x14

    ADDI32 R6, 32
    LOAD64O R7, R5, 0x68
    STORE64O R6, R7, 0x00
    LOAD64O R8, R5, 0x10
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
    MOVI32U R0, 1
    RET
build_boot_info_fail:
    MOVI32U R0, 0
    RET

; ---------------------------------------------------------------------------
; Small runtime helpers

; R0=source, R1=destination, R2=count.
copy_bytes:
    CMPI32 R2, 0
    BRCC EQ, copy_bytes_done
copy_bytes_loop:
    LOAD8U R3, R0
    STORE8 R1, R3
    ADDI32 R0, 1
    ADDI32 R1, 1
    ADDI32 R2, -1
    BRCC NE, copy_bytes_loop
copy_bytes_done:
    RET

; R0=destination, R1=count.
zero_bytes:
    MOVI32U R2, 0
    CMPI32 R1, 0
    BRCC EQ, zero_bytes_done
zero_bytes_loop:
    STORE8 R0, R2
    ADDI32 R0, 1
    ADDI32 R1, -1
    BRCC NE, zero_bytes_loop
zero_bytes_done:
    RET

; Standard reflected CRC-32/ISO-HDLC. R0=data, R1=size, result in R0.
crc32:
    MOVI64 R2, 0xFFFFFFFF
crc32_byte_loop:
    CMPI32 R1, 0
    BRCC EQ, crc32_done
    LOAD8U R3, R0
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
    ADDI32 R0, 1
    ADDI32 R1, -1
    JUMPREL crc32_byte_loop
crc32_done:
    NOT R2
    ZEXT32 R0, R2
    RET

; R0=NUL-terminated ROM/RAM string.
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

; ---------------------------------------------------------------------------
; Fatal paths

error_ram:
    MOVI64 R0, message_ram
    JUMPREL fatal
error_device:
    MOVI64 R0, message_device
    JUMPREL fatal
error_gpt:
    MOVI64 R0, message_gpt
    JUMPREL fatal
error_fat:
    MOVI64 R0, message_fat
    JUMPREL fatal
error_io:
    MOVI64 R0, message_io
    JUMPREL fatal
error_bootdir:
    MOVI64 R0, message_bootdir
    JUMPREL fatal
error_kernel_file:
    MOVI64 R0, message_kernel_file
    JUMPREL fatal
error_chain:
    MOVI64 R0, message_chain
    JUMPREL fatal
error_kernel:
    MOVI64 R0, message_kernel
    JUMPREL fatal
error_bootinfo:
    MOVI64 R0, message_bootinfo
fatal:
    CALLREL uart_puts
    HALT

message_start:
    .asciz "RISC-VM ROM: start\n"
message_handoff:
    .asciz "RISC-VM ROM: bootloader\n"
message_ram:
    .asciz "RISC-VM ROM E01: RAM requires 1 MiB\n"
message_device:
    .asciz "RISC-VM ROM E02: no VIO block device\n"
message_gpt:
    .asciz "RISC-VM ROM E03: invalid GPT\n"
message_gpt_header_crc:
    .asciz "RISC-VM ROM E03A: GPT header CRC\n"
message_gpt_entries_crc:
    .asciz "RISC-VM ROM E03B: GPT entries CRC\n"
message_fat:
    .asciz "RISC-VM ROM E04: invalid FAT32\n"
message_io:
    .asciz "RISC-VM ROM E05: block read failed\n"
message_bootdir:
    .asciz "RISC-VM ROM E06: BOOT directory missing\n"
message_kernel_file:
    .asciz "RISC-VM ROM E07: BOOT.EXF missing\n"
message_chain:
    .asciz "RISC-VM ROM E08: invalid FAT chain\n"
message_chain_ram:
    .asciz "RISC-VM ROM E08A: staging exceeds RAM\n"
message_chain_io:
    .asciz "RISC-VM ROM E08B: kernel read failed\n"
message_chain_next:
    .asciz "RISC-VM ROM E08C: FAT lookup failed\n"
message_chain_early:
    .asciz "RISC-VM ROM E08D: early end of chain\n"
message_chain_last:
    .asciz "RISC-VM ROM E08E: missing end of chain\n"
message_kernel:
    .asciz "RISC-VM ROM E09: invalid BOOT.EXF image\n"
message_bootinfo:
    .asciz "RISC-VM ROM E10: cannot build BootInfo\n"
