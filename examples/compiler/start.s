.section .text
.global compiler_demo_entry
.extern compiler_demo_main
.type compiler_demo_entry, function
.type compiler_demo_main, function
.entry compiler_demo_entry

compiler_demo_entry:
    MOVI64 SP, 0x100000
    CALLREL compiler_demo_main
    HALT
.size compiler_demo_entry, $ - compiler_demo_entry
