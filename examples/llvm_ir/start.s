.section .text
.global cvmir_demo_entry
.extern checked_add
.type cvmir_demo_entry, function
.type checked_add, function
.entry cvmir_demo_entry

cvmir_demo_entry:
    MOVI64 SP, 0x100000
    MOVI64 R0, 19
    MOVI64 R1, 23
    CALLREL checked_add
    HALT
.size cvmir_demo_entry, $ - cvmir_demo_entry
