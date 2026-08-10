; Minimal kernel.cvm packaging example. R0 contains CvmBootInfo at handoff.

.entry kernel_entry

kernel_entry:
    MOVI32U SP, 0x20000
    MOVI32U R3, 42
    HALT
