CVM benchmark suite
===================

Copy these files into the project root, next to the build/ directory:
  bench_alu.asm
  bench_branch.asm
  bench_memory.asm
  bench_mmu.asm
  run_bench_suite.ps1

Run once:
  .\run_bench_suite.ps1

Run three times per test and report the median:
  .\run_bench_suite.ps1 -Runs 3

The script uses 1 MiB RAM by default because bench_mmu.asm places page tables
at physical 0x10000, 0x11000 and 0x12000.

Interpretation:
  ALU       - mostly ADDI32 execution/decode throughput
  Branch    - tight branch-heavy loop (same style as the first benchmark)
  RAM       - repeated LOAD64/STORE64 with MMU disabled
  RAM + MMU - same memory workload after enabling a 3-level identity mapping

Caution:
  These are guest-instruction throughput figures, not equivalent host CPU MIPS.
  Different CVM opcodes have different host-side costs.
