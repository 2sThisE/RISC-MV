$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Vm  = Join-Path $ProjectRoot 'build\main.exe'
$Bin = Join-Path $ProjectRoot 'build\examples\bench_loop.bin'

& (Join-Path $ProjectRoot 'build.ps1') -e benchmarks -nc
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

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
