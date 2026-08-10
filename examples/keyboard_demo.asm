; VIO HID keyboard guest-driver demonstration.
; Discovers the input device through the VIO Hub, installs its IRQ vector,
; and prints key events to UART as D:04 / U:04. Escape key-down halts.

.entry start

start:
    MOVI32U SP, 0xFF0

    ; Enable UART TX and keep TXDATA in R12.
    MOVI64 R12, 0xFFFFFFFFFFFD0000
    MOVI64 R1, 0xFFFFFFFFFFFD0018
    MOVI32U R2, 1
    STORE64 R1, R2

    ; VIO Hub: scan every present slot for class INPUT (7).
    MOVI64 R14, 0xFFFFFFFFFFFC0000
    LOAD64O R13, R14, 0x10       ; SLOT_COUNT
    MOVI32U R0, 0

scan_slot:
    CMP R0, R13
    BRCC GEU, no_keyboard

    MOV R1, R0
    MOVI32U R2, 0xC0
    MUL R1, R2
    ADDI32 R1, 0x100
    ADD R1, R14                  ; R1 = slot configuration address

    LOAD64O R2, R1, 0x00        ; STATUS
    TESTI32 R2, 1
    BRCC EQ, next_slot
    LOAD64O R2, R1, 0x08        ; CLASS
    CMPI32 R2, 7
    BRCC EQ, keyboard_found

next_slot:
    ADDI32 R0, 1
    JUMPREL scan_slot

keyboard_found:
    LOAD64O R10, R1, 0x28       ; BAR0_BASE
    LOAD64O R11, R1, 0x38       ; IRQ0

    ; Install keyboard_irq at vector_table + IRQ * 8.
    MOV R3, R11
    SHL R3, 3
    MOVI64 R4, vector_table
    ADD R3, R4
    MOVI64 R5, keyboard_irq
    STORE64 R3, R5
    SETVBR R4

    ; Route defaults to logical processor 0. Unmask the discovered IRQ.
    MOVI32U R2, 1
    SHLV R2, R11
    MOVI64 R3, 0xFFFFFFFFFFFC3028 ; IRQ Controller ENABLE_SET
    STORE64 R3, R2

    ; Enable keyboard and its IRQ.
    MOVI32U R2, 3
    STORE64O R10, R2, 0x00

    MOVI32U R2, 75              ; 'K': keyboard ready
    STORE8 R12, R2
    MOVI32U R2, 10
    STORE8 R12, R2
    EI

idle:
    WAIT
    JUMPREL idle

no_keyboard:
    MOVI32U R2, 78              ; 'N': no keyboard
    STORE8 R12, R2
    MOVI32U R2, 10
    STORE8 R12, R2
    HALT

keyboard_irq:
    PUSH R0
    PUSH R1
    PUSH R2
    PUSH R3
    PUSH R4
    PUSH R5
    PUSH R6
    PUSH R7
    PUSH R10
    PUSH R12

drain_events:
    LOAD64O R0, R10, 0x10       ; EVENT_COUNT
    CMPI32 R0, 0
    BRCC EQ, irq_done
    LOAD64O R1, R10, 0x18       ; EVENT_DATA (pop)

    MOV R2, R1
    ANDI32 R2, 0xFFFF           ; HID usage
    MOVI32U R7, 0               ; halt after printing this event?
    TESTI32 R1, 0x10000         ; key-down bit
    BRCC EQ, key_up

key_down:
    MOVI32U R3, 68              ; 'D'
    CMPI32 R2, 0x29             ; Escape HID usage
    BRCC NE, print_event
    MOVI32U R7, 1
    JUMPREL print_event

key_up:
    MOVI32U R3, 85              ; 'U'

print_event:
    STORE8 R12, R3
    MOVI32U R3, 58              ; ':'
    STORE8 R12, R3

    MOV R3, R2
    SHR R3, 4
    ANDI32 R3, 0xF
    CALLREL print_hex_digit
    MOV R3, R2
    ANDI32 R3, 0xF
    CALLREL print_hex_digit
    MOVI32U R3, 10
    STORE8 R12, R3

    CMPI32 R7, 0
    BRCC NE, stop_vm
    JUMPREL drain_events

irq_done:
    ; Clear EVENT and OVERFLOW IRQ status, then clear overflow status.
    MOVI32U R0, 3
    STORE64O R10, R0, 0x28
    MOVI32U R0, 2
    STORE64O R10, R0, 0x08

    ; Complete the controller-side interrupt after the device ACK.
    MOVI64 R0, 0xFFFFFFFFFFFC3058 ; IRQ Controller EOI
    STORE64 R0, R11

    POP R12
    POP R10
    POP R7
    POP R6
    POP R5
    POP R4
    POP R3
    POP R2
    POP R1
    POP R0
    IRET

stop_vm:
    HALT

print_hex_digit:
    CMPI32 R3, 10
    BRCC LTU, hex_number
    ADDI32 R3, 55               ; A-F
    JUMPREL hex_write

hex_number:
    ADDI32 R3, 48               ; 0-9

hex_write:
    STORE8 R12, R3
    RET

; 77 entries, initialized to UINT64_MAX. The discovered IRQ entry is
; overwritten before SETVBR/EI.
.org 0x800
vector_table:
    .space 616, 0xFF
