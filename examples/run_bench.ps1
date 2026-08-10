$ErrorActionPreference = "Stop"

$Asm = "..\build\vmasm.exe"
$Vm  = "..\build\main.exe"
$Src = ".\bench_loop.asm"
$Bin = ".\bench_loop.bin"

& $Asm $Src -o $Bin --symbols ".\bench_loop.sym" --listing ".\bench_loop.lst"
if ($LASTEXITCODE -ne 0) { throw "Assembler failed: $LASTEXITCODE" }

$instructionCount = 200000002.0

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $Vm -r 4096 -l $Bin
$exitCode = $LASTEXITCODE
$sw.Stop()

if ($exitCode -ne 0) { throw "VM failed: $exitCode" }

$seconds = $sw.Elapsed.TotalSeconds
$mips = ($instructionCount / $seconds) / 1000000.0
$nsPerInstruction = ($seconds * 1000000000.0) / $instructionCount

Write-Host ""
Write-Host ("Elapsed              : {0:N6} s" -f $seconds)
Write-Host ("Guest instructions   : {0:N0}" -f $instructionCount)
Write-Host ("Approx throughput     : {0:N2} MIPS" -f $mips)
Write-Host ("Approx ns/instruction : {0:N2} ns" -f $nsPerInstruction)
