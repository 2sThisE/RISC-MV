param(
    [int]$Runs = 1,
    [int64]$RamBytes = 1048576
)

$ErrorActionPreference = "Stop"

$Asm = "..\build\vmasm.exe"
$Vm  = "..\build\main.exe"

$tests = @(
    [pscustomobject]@{ Name = "ALU";       Source = ".\bench_alu.asm";    Instructions = 180000003.0 },
    [pscustomobject]@{ Name = "Branch";    Source = ".\bench_branch.asm"; Instructions = 200000002.0 },
    [pscustomobject]@{ Name = "RAM";       Source = ".\bench_memory.asm"; Instructions = 180000005.0 },
    [pscustomobject]@{ Name = "RAM + MMU"; Source = ".\bench_mmu.asm";    Instructions = 180000017.0 }
)

if (-not (Test-Path $Asm)) { throw "Assembler not found: $Asm" }
if (-not (Test-Path $Vm))  { throw "VM not found: $Vm" }
if ($Runs -lt 1) { throw "Runs must be at least 1" }

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $n = $sorted.Count
    if (($n % 2) -eq 1) {
        return [double]$sorted[[int]($n / 2)]
    }
    return ([double]$sorted[$n / 2 - 1] + [double]$sorted[$n / 2]) / 2.0
}

$results = @()

foreach ($test in $tests) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($test.Source)
    $bin  = ".\$base.bin"
    $sym  = ".\$base.sym"
    $lst  = ".\$base.lst"

    Write-Host ""
    Write-Host "=== $($test.Name) ==="
    Write-Host "Assembling $($test.Source)"

    & $Asm $test.Source -o $bin --symbols $sym --listing $lst
    if ($LASTEXITCODE -ne 0) { throw "Assembler failed for $($test.Source): $LASTEXITCODE" }

    $times = @()
    for ($i = 1; $i -le $Runs; $i++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $Vm -r $RamBytes -l $bin
        $exitCode = $LASTEXITCODE
        $sw.Stop()

        if ($exitCode -ne 0) { throw "VM failed for $($test.Name): $exitCode" }

        $seconds = $sw.Elapsed.TotalSeconds
        $times += $seconds
        Write-Host ("Run {0}/{1}: {2:N6} s" -f $i, $Runs, $seconds)
    }

    $medianSeconds = Get-Median $times
    $mips = ($test.Instructions / $medianSeconds) / 1000000.0
    $nsPerInstruction = ($medianSeconds * 1000000000.0) / $test.Instructions

    $results += [pscustomobject]@{
        Test = $test.Name
        Seconds = [math]::Round($medianSeconds, 6)
        MIPS = [math]::Round($mips, 2)
        NsPerInstruction = [math]::Round($nsPerInstruction, 2)
    }
}

Write-Host ""
Write-Host "=== Summary (median if Runs > 1) ==="
$results | Format-Table -AutoSize

$ram = $results | Where-Object Test -eq "RAM"
$mmu = $results | Where-Object Test -eq "RAM + MMU"
if ($ram -and $mmu -and $ram.Seconds -gt 0) {
    $slowdown = $mmu.Seconds / $ram.Seconds
    Write-Host ("MMU slowdown vs RAM benchmark: {0:N2}x" -f $slowdown)
}

Write-Host ""
Write-Host "Tip: use .\run_bench_suite.ps1 -Runs 3 for a more stable median."
