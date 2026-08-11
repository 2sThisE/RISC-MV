; RISC-MV second-stage bootloader.
; Entry handoff:
;   R0 = CvmFirmwareTable at 0x6000
;   R1 = "CVMFWH01" handoff magic
;   R2 = boot-device slot

.entry loader_entry

loader_entry:
    DI
    ; The second-stage owns the fixed 0x20000..0x30000 bootloader slot.
    MOVI32U SP, 0x30000
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
    MOVI32U R1, 22
    CALLREL firmware_console
    CMPI32 R0, 0
    BRCC NE, loader_fatal_halt

    ; Ask firmware for KERNEL.EXF size. Reserve only its page-rounded size at
    ; the top of RAM. The relocatable kernel itself is placed first-fit at
    ; physical first-fit at or above 0x30000 and executes at its fixed VA.
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
    ADDI32 R4, 4095
    CMP R4, R1
    BRCC LTU, loader_fatal_file
    MOVI64 R5, 0xFFFFFFFFFFFFF000
    AND R4, R5
    LOAD64O R5, R10, 0x20
    CMP R4, R5
    BRCC GTU, loader_fatal_file
    SUB R5, R4
    MOVI64 R6, 0xFFFFFFFFFFFFF000
    AND R5, R6
    CMPI32 R5, 0x30000
    BRCC LTU, loader_fatal_file
    STORE64O R3, R5, 0x30
    MOVI64 R0, kernel_name
    MOV R1, R5
    LOAD64O R2, R3, 0x08
    LOAD64O R11, R10, 0xB8
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_file

    ; Select the kernel physical base from the firmware's current USABLE map.
    MOVI32U R0, 0x8400
    MOVI32U R1, 2
    LOAD64O R11, R10, 0xA8
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_map
    MOVI32U R0, 0x8400
    CALLREL select_kernel_physical_base
    CMPI32 R0, 0
    BRCC NE, loader_fatal_kernel_validate

    CALLREL validate_kernel_image
    CMPI32 R0, 0
    BRCC NE, loader_fatal_kernel_validate
    CALLREL load_kernel_image
    CMPI32 R0, 0
    BRCC NE, loader_fatal_kernel_load
    CALLREL build_initial_page_tables
    CMPI32 R0, 0
    BRCC NE, loader_fatal_kernel_mmu

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
    MOVI32U R1, 23
    CALLREL firmware_console
    CMPI32 R0, 0
    BRCC NE, loader_fatal_halt

    MOVI32U R3, 0x9000
    LOAD64O R0, R3, 0x28
    LOAD64O R11, R10, 0xC0
    CALLR R11
    CMPI32 R0, 0
    BRCC NE, loader_fatal_halt

    ; Services are now unavailable. Install the initial page table while the
    ; loader remains identity mapped, then enter the fixed virtual kernel.
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
    MOVI32U R14, 0x9000
    LOAD64O R14, R14, 0x68
    SETPTBR R14
    MOVI32U R14, 0
    MOVI32U SP, 0x30000
    MMUON
    JUMP 0x7000

; Calls firmware ConsoleWrite while preserving the table in R10.
firmware_console:
    LOAD64O R11, R10, 0xA0
    CALLR R11
    RET

; ---------------------------------------------------------------------------
; RISC-MV KERNEL.EXF v1 multi-segment validation

; R0=firmware memory-map buffer, R1=entry count. Chooses the first page-aligned
; USABLE range after the fixed second-stage slot that also stays below staging.
select_kernel_physical_base:
    PUSH R10
    MOV R3, R0
    MOV R4, R1
select_kernel_physical_base_loop:
    CMPI32 R4, 0
    BRCC EQ, select_kernel_physical_base_fail
    LOAD32UO R5, R3, 0x10
    CMPI32 R5, 1
    BRCC NE, select_kernel_physical_base_next
    LOAD64O R5, R3, 0x00
    LOAD64O R6, R3, 0x08
    MOV R7, R5
    ADD R7, R6
    CMP R7, R5
    BRCC LTU, select_kernel_physical_base_next
    CMPI32 R5, 0x30000
    BRCC GEU, select_kernel_physical_base_align
    MOVI32U R5, 0x30000
select_kernel_physical_base_align:
    ADDI32 R5, 4095
    MOVI64 R8, 0xFFFFFFFFFFFFF000
    AND R5, R8
    MOVI32U R9, 0x9000
    LOAD64O R8, R9, 0x30
    CMP R5, R8
    BRCC GEU, select_kernel_physical_base_next
    LOAD64O R10, R9, 0x30
    LOAD64O R6, R10, 0x70
    CMPI32 R6, 0
    BRCC EQ, select_kernel_physical_base_next
    MOV R10, R5
    ADD R10, R6
    CMP R10, R5
    BRCC LTU, select_kernel_physical_base_next
    CMP R10, R7
    BRCC GTU, select_kernel_physical_base_next
    LOAD64O R8, R9, 0x30
    CMP R10, R8
    BRCC GTU, select_kernel_physical_base_next
    STORE64O R9, R5, 0x18
    STORE64O R9, R5, 0x20
    MOVI32U R0, 0
    POP R10
    RET
select_kernel_physical_base_next:
    ADDI32 R3, 32
    ADDI32 R4, -1
    JUMPREL select_kernel_physical_base_loop
select_kernel_physical_base_fail:
    MOVI32U R0, 1
    POP R10
    RET

validate_kernel_image:
    MOVI32U R6, 0x9000
    MOVI32U R4, 1
    STORE64O R6, R4, 0x88
    LOAD64O R3, R6, 0x30
    LOAD64 R4, R3
    MOVI64 R5, 0x3130465845564D52 ; "RMVEXF01"
    CMP R4, R5
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x08
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x0C
    CMPI32 R4, 128
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x10
    CMPI32 R4, 1               ; RELOCATABLE_PHYSICAL
    BRCC NE, validate_kernel_fail
    LOAD32UO R4, R3, 0x18
    MOVI32U R5, 0x34364152        ; "RA64"
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

    ; Relocatable header extension.
    LOAD64O R4, R3, 0x60       ; virtual entry
    LOAD64O R5, R3, 0x68       ; virtual base
    CMP R4, R5
    BRCC LTU, validate_kernel_fail
    MOV R7, R4
    SUB R7, R5
    LOAD64O R8, R3, 0x30       ; physical entry offset
    CMP R7, R8
    BRCC NE, validate_kernel_fail
    LOAD64O R6, R3, 0x70       ; virtual span
    CMPI32 R6, 0
    BRCC EQ, validate_kernel_fail
    MOV R7, R5
    ADD R7, R6
    CMP R7, R5
    BRCC LTU, validate_kernel_fail
    MOVI64 R8, 0x8000000000    ; 39-bit virtual limit
    CMP R7, R8
    BRCC GTU, validate_kernel_fail
    LOAD64O R7, R3, 0x78
    CMPI32 R7, 0
    BRCC NE, validate_kernel_fail
    MOVI32U R9, 0x9000
    STORE64O R9, R4, 0x10
    STORE64O R9, R5, 0x58
    STORE64O R9, R6, 0x60
    MOVI32U R4, 2
    STORE64O R9, R4, 0x88

    ; Header CRC.
    LOAD32UO R4, R3, 0x58
    MOVI32U R6, 0x9000
    STORE64O R6, R4, 0x40
    MOVI32U R5, 0
    STORE32O R3, R5, 0x58
    MOV R0, R3
    MOVI32U R1, 128
    CALLREL crc32
    MOVI32U R6, 0x9000
    LOAD64O R4, R6, 0x40
    LOAD64O R3, R6, 0x30
    STORE32O R3, R4, 0x58
    CMP R0, R4
    BRCC NE, validate_kernel_fail
    MOVI32U R6, 0x9000
    MOVI32U R4, 3
    STORE64O R6, R4, 0x88

    ; Payload CRC.
    LOAD64O R0, R6, 0x30
    ADDI32 R0, 128
    LOAD64O R1, R6, 0x08
    ADDI32 R1, -128
    CALLREL crc32
    LOAD64O R3, R6, 0x30
    LOAD32UO R4, R3, 0x5C
    CMP R0, R4
    BRCC NE, validate_kernel_fail
    MOVI32U R9, 0x9000
    MOVI32U R4, 4
    STORE64O R9, R4, 0x88

    ; Validate sorted, non-overlapping load segments.
    MOVI32U R9, 0x9000
    LOAD64O R4, R9, 0x18      ; firmware-map first-fit physical base
    STORE64O R9, R4, 0x20
    MOVI32U R4, 0
    STORE64O R9, R4, 0x50
    LOAD64O R13, R9, 0x30
    ADDI32 R13, 128
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
    LOAD64O R5, R3, 0x10      ; physical offset
    LOAD64O R11, R3, 0x18     ; fixed virtual address
    LOAD64O R4, R9, 0x58
    MOV R14, R4
    ADD R14, R5
    CMP R14, R4
    BRCC LTU, validate_kernel_fail
    CMP R11, R14
    BRCC NE, validate_kernel_fail
    ADD R14, R7
    CMP R14, R11
    BRCC LTU, validate_kernel_fail
    LOAD64O R4, R9, 0x58
    LOAD64O R8, R9, 0x60
    ADD R8, R4
    CMP R14, R8
    BRCC GTU, validate_kernel_fail
    LOAD64O R4, R3, 0x30
    CMPI32 R4, 0
    BRCC EQ, validate_kernel_fail
    MOV R11, R4
    ADDI32 R11, -1
    MOV R14, R4
    AND R14, R11
    CMPI32 R14, 0
    BRCC NE, validate_kernel_fail
    MOV R8, R5
    AND R8, R11
    CMPI32 R8, 0
    BRCC NE, validate_kernel_fail
    LOAD64O R8, R3, 0x18
    AND R8, R11
    CMPI32 R8, 0
    BRCC NE, validate_kernel_fail
    LOAD64O R4, R3, 0x38
    CMPI32 R4, 0
    BRCC NE, validate_kernel_fail

    ; Convert the offset to a physical destination and keep segments sorted.
    LOAD64O R8, R9, 0x18
    ADD R8, R5
    CMP R8, R5
    BRCC LTU, validate_kernel_fail
    MOV R14, R8
    ADD R14, R7
    CMP R14, R8
    BRCC LTU, validate_kernel_fail
    LOAD64O R4, R9, 0x20
    CMP R8, R4
    BRCC LTU, validate_kernel_fail
    LOAD64O R4, R9, 0x30
    CMP R14, R4
    BRCC GTU, validate_kernel_fail
    STORE64O R9, R14, 0x20
    LOAD32UO R4, R3, 0x04
    TESTI32 R4, 4
    BRCC EQ, validate_segment_not_entry
    LOAD64O R4, R9, 0x10
    LOAD64O R11, R3, 0x18
    CMP R4, R11
    BRCC LTU, validate_segment_not_entry
    LOAD64O R8, R3, 0x28
    ADD R8, R11
    CMP R4, R8
    BRCC GEU, validate_segment_not_entry
    MOVI32U R4, 1
    STORE64O R9, R4, 0x50
validate_segment_not_entry:
    ADDI32 R13, 64
    ADDI32 R12, -1
    BRCC NE, validate_segment_loop
    LOAD64O R4, R9, 0x50
    CMPI32 R4, 1
    BRCC NE, validate_kernel_fail
    MOVI32U R4, 5
    STORE64O R9, R4, 0x88
    MOVI32U R0, 0
    RET
validate_kernel_fail:
    MOVI32U R0, 0x9000
    LOAD64O R0, R0, 0x88
    RET

load_kernel_image:
    MOVI32U R3, 0x9000
    LOAD64O R3, R3, 0x30
    LOAD16UO R12, R3, 0x22
    MOV R13, R3
    ADDI32 R13, 128
load_segment_loop:
    LOAD64O R0, R13, 0x08
    MOVI32U R4, 0x9000
    LOAD64O R4, R4, 0x30
    ADD R0, R4
    LOAD64O R1, R13, 0x10
    MOVI32U R4, 0x9000
    LOAD64O R4, R4, 0x18
    ADD R1, R4
    LOAD64O R2, R13, 0x20
    CALLREL copy_bytes
    LOAD64O R0, R13, 0x10
    MOVI32U R4, 0x9000
    LOAD64O R4, R4, 0x18
    ADD R0, R4
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
; Initial three-level page tables

build_initial_page_tables:
    MOVI32U R9, 0x9000
    LOAD64O R4, R9, 0x20
    ADDI32 R4, 4095
    LOAD64O R5, R9, 0x20
    CMP R4, R5
    BRCC LTU, build_initial_page_tables_fail
    MOVI64 R5, 0xFFFFFFFFFFFFF000
    AND R4, R5
    STORE64O R9, R4, 0x20      ; kernel physical span includes padding
    STORE64O R9, R4, 0x70      ; initial page-table arena base
    STORE64O R9, R4, 0x78      ; allocation cursor
    LOAD64O R5, R9, 0x30
    STORE64O R9, R5, 0x80      ; allocation limit (staging start)
    CALLREL allocate_table_page
    CMPI32 R0, 0
    BRCC EQ, build_initial_page_tables_fail
    MOVI32U R9, 0x9000
    STORE64O R9, R0, 0x68      ; PTBR

    ; Identity-map firmware, BootInfo, trampoline and second-stage loader.
    MOVI32U R0, 0
    MOVI32U R1, 0
    MOVI32U R2, 0x30000
    MOVI32U R3, 14             ; supervisor RWX
    CALLREL map_page_range
    CMPI32 R0, 0
    BRCC NE, build_initial_page_tables_fail

    ; Map every kernel segment from its fixed VA to selected physical RAM.
    MOVI32U R9, 0x9000
    LOAD64O R13, R9, 0x30
    LOAD16UO R12, R13, 0x22
    ADDI32 R13, 128
build_initial_kernel_map_loop:
    LOAD64O R0, R13, 0x18
    LOAD64O R1, R13, 0x10
    MOVI32U R9, 0x9000
    LOAD64O R4, R9, 0x18
    ADD R1, R4
    LOAD64O R2, R13, 0x28
    LOAD32UO R3, R13, 0x04
    SHL R3, 1                 ; segment R/W/X -> PTE R/W/X
    CALLREL map_page_range
    CMPI32 R0, 0
    BRCC NE, build_initial_page_tables_fail
    ADDI32 R13, 64
    ADDI32 R12, -1
    BRCC NE, build_initial_kernel_map_loop

    ; A stable direct-map lets the kernel manipulate arbitrary physical pages
    ; immediately, even though it entered at a non-identity virtual address.
    MOVI64 R0, 0x100000000
    MOVI32U R1, 0
    MOVI32U R9, 0x6000
    LOAD64O R2, R9, 0x20
    MOV R5, R0
    ADD R5, R2
    CMP R5, R0
    BRCC LTU, build_initial_page_tables_fail
    MOVI64 R4, 0x8000000000
    CMP R5, R4
    BRCC GTU, build_initial_page_tables_fail
    MOVI64 R4, 0xFFFFFFFFFFFFF000
    AND R2, R4
    MOVI32U R3, 6             ; supervisor RW, never executable
    CALLREL map_page_range
    CMPI32 R0, 0
    BRCC NE, build_initial_page_tables_fail

    ; The physical MMIO address is outside the 39-bit virtual range.
    MOVI64 R0, 0x3FFFF000
    MOVI32U R9, 0x6000
    LOAD64O R1, R9, 0x78
    MOVI32U R2, 4096
    MOVI32U R3, 6
    CALLREL map_page_range
    CMPI32 R0, 0
    BRCC NE, build_initial_page_tables_fail
    MOVI32U R0, 0
    RET
build_initial_page_tables_fail:
    MOVI32U R0, 1
    RET

; R0=VA, R1=PA, R2=byte count, R3=PTE permission bits.
map_page_range:
    PUSH R4
    PUSH R5
    PUSH R6
    PUSH R7
    MOV R4, R0
    MOV R5, R1
    MOV R6, R2
    MOV R7, R3
    CMPI32 R6, 0
    BRCC EQ, map_page_range_done
map_page_range_loop:
    MOV R0, R4
    MOV R1, R5
    MOV R2, R7
    CALLREL map_page
    CMPI32 R0, 0
    BRCC NE, map_page_range_fail
    CMPI32 R6, 4096
    BRCC LEU, map_page_range_done
    ADDI32 R4, 4096
    ADDI32 R5, 4096
    ADDI32 R6, -4096
    JUMPREL map_page_range_loop
map_page_range_done:
    MOVI32U R0, 0
    JUMPREL map_page_range_restore
map_page_range_fail:
    MOVI32U R0, 1
map_page_range_restore:
    POP R7
    POP R6
    POP R5
    POP R4
    RET

; R0=VA, R1=PA, R2=PTE permission bits. Returns zero on success.
map_page:
    PUSH R3
    PUSH R4
    PUSH R5
    PUSH R6
    PUSH R7
    PUSH R8
    PUSH R9
    MOV R3, R0
    MOV R4, R1
    MOV R5, R2
    MOVI32U R6, 0x9000
    LOAD64O R6, R6, 0x68

    MOV R7, R3
    SHR R7, 30
    ANDI32 R7, 0x1FF
    SHL R7, 3
    ADD R7, R6
    LOAD64 R8, R7
    TESTI32 R8, 1
    BRCC NE, map_page_have_level1
    CALLREL allocate_table_page
    CMPI32 R0, 0
    BRCC EQ, map_page_fail
    MOV R8, R0
    ORI32 R8, 1
    STORE64 R7, R8
map_page_have_level1:
    MOVI64 R9, 0xFFFFFFFFFFFFF000
    AND R8, R9
    MOV R7, R3
    SHR R7, 21
    ANDI32 R7, 0x1FF
    SHL R7, 3
    ADD R7, R8
    LOAD64 R8, R7
    TESTI32 R8, 1
    BRCC NE, map_page_have_level0
    CALLREL allocate_table_page
    CMPI32 R0, 0
    BRCC EQ, map_page_fail
    MOV R8, R0
    ORI32 R8, 1
    STORE64 R7, R8
map_page_have_level0:
    MOVI64 R9, 0xFFFFFFFFFFFFF000
    AND R8, R9
    MOV R7, R3
    SHR R7, 12
    ANDI32 R7, 0x1FF
    SHL R7, 3
    ADD R7, R8
    MOVI64 R9, 0xFFFFFFFFFFFFF000
    AND R4, R9
    OR R4, R5
    ORI32 R4, 1
    STORE64 R7, R4
    MOVI32U R0, 0
    JUMPREL map_page_restore
map_page_fail:
    MOVI32U R0, 1
map_page_restore:
    POP R9
    POP R8
    POP R7
    POP R6
    POP R5
    POP R4
    POP R3
    RET

; Allocates and clears one physical page from the temporary PT arena.
allocate_table_page:
    PUSH R1
    PUSH R2
    PUSH R3
    PUSH R4
    PUSH R5
    MOVI32U R1, 0x9000
    LOAD64O R0, R1, 0x78
    MOV R2, R0
    ADDI32 R2, 4096
    CMP R2, R0
    BRCC LTU, allocate_table_page_fail
    LOAD64O R3, R1, 0x80
    CMP R2, R3
    BRCC GTU, allocate_table_page_fail
    STORE64O R1, R2, 0x78
    MOV R4, R0
    MOVI32U R5, 4096
    MOVI32U R3, 0
allocate_table_page_zero_loop:
    STORE8 R4, R3
    ADDI32 R4, 1
    ADDI32 R5, -1
    BRCC NE, allocate_table_page_zero_loop
    JUMPREL allocate_table_page_restore
allocate_table_page_fail:
    MOVI32U R0, 0
allocate_table_page_restore:
    POP R5
    POP R4
    POP R3
    POP R2
    POP R1
    RET

; ---------------------------------------------------------------------------
; Kernel BootInfo

build_boot_info:
    MOVI32U R0, 0x8000
    MOVI32U R1, 512
    CALLREL zero_bytes
    MOVI32U R3, 0x8000
    MOVI64 R4, 0x31544F4F424D5643
    STORE64 R3, R4
    MOVI32U R4, 1
    STORE32O R3, R4, 0x08
    MOVI32U R4, 256
    STORE32O R3, R4, 0x0C
    MOVI32U R4, 512
    STORE32O R3, R4, 0x10
    MOVI32U R4, 17             ; MMU_ENABLED | GPT_BOOT
    STORE32O R3, R4, 0x14
    MOVI32U R4, 320            ; header + virtual handoff extension
    STORE64O R3, R4, 0x20
    MOVI32U R4, 6
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

    ; CvmBootVirtualHandoff at BootInfo+0x100.
    MOVI32U R6, 0x8100
    MOVI64 R7, 0x31545249564D5643 ; "CVMVIRT1"
    STORE64O R6, R7, 0x00
    MOVI32U R5, 0x9000
    LOAD64O R7, R5, 0x58
    STORE64O R6, R7, 0x08
    LOAD64O R7, R5, 0x60
    STORE64O R6, R7, 0x10
    LOAD64O R7, R5, 0x68
    STORE64O R6, R7, 0x18
    MOVI64 R7, 0x100000000
    STORE64O R6, R7, 0x20
    MOVI32U R10, 0x6000
    LOAD64O R7, R10, 0x20
    STORE64O R6, R7, 0x28
    LOAD64O R7, R5, 0x70
    STORE64O R6, R7, 0x30
    LOAD64O R8, R5, 0x78
    SUB R8, R7
    STORE64O R6, R8, 0x38

    ; Six sorted, non-overlapping physical memory-map entries.
    MOVI32U R6, 0x8140
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
    MOVI32U R7, 0x1000
    STORE64O R6, R7, 0x08
    MOVI32U R7, 5
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    MOVI32U R7, 0x9000
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
    LOAD64O R8, R5, 0x70
    SUB R8, R7
    STORE64O R6, R8, 0x08
    MOVI32U R7, 3
    STORE32O R6, R7, 0x10
    MOVI32U R7, 7
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    LOAD64O R7, R5, 0x70
    STORE64O R6, R7, 0x00
    LOAD64O R8, R5, 0x78
    SUB R8, R7
    CMPI32 R8, 0
    BRCC EQ, build_boot_info_fail
    STORE64O R6, R8, 0x08
    MOVI32U R7, 4
    STORE32O R6, R7, 0x10
    MOVI32U R7, 3
    STORE32O R6, R7, 0x14
    ADDI32 R6, 32
    LOAD64O R7, R5, 0x78
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
    MOVI32U R1, 512
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
    MOVI32U R1, 29
    JUMPREL loader_fatal_print
loader_fatal_file:
    MOVI64 R0, loader_message_file
    MOVI32U R1, 31
    JUMPREL loader_fatal_print
loader_fatal_kernel_validate:
    MOVI64 R4, loader_message_bad_kernel_validate
    ADDI32 R0, 48
    STORE8O R4, R0, 35
    MOVI64 R0, loader_message_bad_kernel_validate
    MOVI32U R1, 37
    JUMPREL loader_fatal_print
loader_fatal_kernel_load:
    MOVI64 R0, loader_message_bad_kernel_load
    MOVI32U R1, 32
    JUMPREL loader_fatal_print
loader_fatal_kernel_mmu:
    MOVI64 R0, loader_message_bad_kernel_mmu
    MOVI32U R1, 31
    JUMPREL loader_fatal_print
loader_fatal_map:
    MOVI64 R0, loader_message_map
    MOVI32U R1, 31
loader_fatal_print:
    MOVI32U R10, 0x6000
    CALLREL firmware_console
loader_fatal_halt:
    HALT

kernel_name:
    .ascii "KERNEL  EXF"
loader_message_start:
    .ascii "RISC-MV LOADER: start\n"
loader_message_kernel:
    .ascii "RISC-MV LOADER: kernel\n"
loader_message_table:
    .ascii "RISC-MV LOADER E01: firmware\n"
loader_message_file:
    .ascii "RISC-MV LOADER E02: KERNEL.EXF\n"
loader_message_bad_kernel_validate:
    .ascii "RISC-MV LOADER E03: validate stage 0\n"
loader_message_bad_kernel_load:
    .ascii "RISC-MV LOADER E04: kernel load\n"
loader_message_bad_kernel_mmu:
    .ascii "RISC-MV LOADER E05: kernel MMU\n"
loader_message_map:
    .ascii "RISC-MV LOADER E06: memory map\n"
