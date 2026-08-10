param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InputFile,

    [Alias('o')]
    [Parameter(Mandatory = $true)]
    [string]$OutputFile,

    [Alias('S')]
    [switch]$EmitAssembly,

    [Alias('c')]
    [switch]$CompileOnly,

    [string]$Startup,
    [string]$Entry = '_start',
    [UInt64]$Base = 0x10000,
    [string[]]$IncludeDirectory
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Program, [string[]]$Arguments,
                        [string]$Description) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

if ($EmitAssembly -and $CompileOnly) {
    throw '-S and -c are mutually exclusive'
}
$inputPath = (Resolve-Path -LiteralPath $InputFile).Path
$toolDirectory = $PSScriptRoot
$cvmir = Join-Path $toolDirectory 'cvmir.exe'
$assembler = Join-Path $toolDirectory 'vmasm.exe'
$linker = Join-Path $toolDirectory 'cvmlink.exe'
$clang = (Get-Command clang -ErrorAction Stop).Source
foreach ($tool in @($cvmir, $assembler, $linker)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "required CVM tool is missing: $tool"
    }
}

$projectRoot = Split-Path (Split-Path $toolDirectory -Parent) -Parent
$sourceSysroot = Join-Path $projectRoot 'tools\ir_translator\sysroot\include'
$builtSysroot = Join-Path (Split-Path $toolDirectory -Parent) 'sysroot\include'
$sysrootInclude = if (Test-Path -LiteralPath $builtSysroot) {
    $builtSysroot
} elseif (Test-Path -LiteralPath $sourceSysroot) {
    $sourceSysroot
} else {
    throw 'CVM sysroot headers were not found'
}
$sysrootLibrary = Join-Path (Split-Path $toolDirectory -Parent) 'sysroot\lib'
$defaultStartup = Join-Path $sysrootLibrary 'crt0.o'
$runtimeArchive = Join-Path $sysrootLibrary 'libcvm.a'

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('cvmclang-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try {
    $hostIr = Join-Path $temporaryRoot 'input.host.ll'
    $normalizedIr = Join-Path $temporaryRoot 'input.cvm.ll'
    $object = Join-Path $temporaryRoot 'input.o'
    $clangArguments = @(
        '--target=x86_64-unknown-none-elf',
        '-mlong-double-64',
        '-S', '-emit-llvm', '-O0',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fno-pic', '-fno-pie',
        '-nostdinc', '-isystem', $sysrootInclude,
        '-D__CVM__=1', '-D__cvm64__=1'
    )
    foreach ($directory in $IncludeDirectory) {
        $clangArguments += @('-I', $directory)
    }
    $clangArguments += @($inputPath, '-o', $hostIr)
    Invoke-Checked -Program $clang -Arguments $clangArguments `
        -Description 'Clang LLVM IR generation'

    $content = [System.IO.File]::ReadAllText($hostIr)
    $triple = 'target triple = "cvm64-unknown-none"'
    $layout = 'target datalayout = "e-p:64:64-i8:8-i16:16-i32:32-i64:64-f32:32-f64:64-v128:128-a:0:64-n8:16:32:64-S128"'
    $content = [regex]::Replace($content, '(?m)^target triple = .+$', $triple)
    $content = [regex]::Replace($content, '(?m)^target datalayout = .+$', $layout)
    [System.IO.File]::WriteAllText($normalizedIr, $content,
        [System.Text.UTF8Encoding]::new($false))

    if ($EmitAssembly) {
        Invoke-Checked -Program $cvmir `
            -Arguments @('-S', $normalizedIr, '-o', $OutputFile) `
            -Description 'CVM assembly generation'
        return
    }
    Invoke-Checked -Program $cvmir `
        -Arguments @('-c', $normalizedIr, '-o', $object) `
        -Description 'CVM object generation'
    if ($CompileOnly) {
        Copy-Item -LiteralPath $object -Destination $OutputFile -Force
        return
    }

    $startupObject = $defaultStartup
    if ([string]::IsNullOrWhiteSpace($Startup)) {
        if (-not (Test-Path -LiteralPath $defaultStartup)) {
            throw "default CVM startup object is missing: $defaultStartup"
        }
    } else {
        $assembledStartup = Join-Path $temporaryRoot 'startup.o'
        Invoke-Checked -Program $assembler `
            -Arguments @($Startup, '-c', '-o', $assembledStartup) `
            -Description 'CVM startup assembly'
        $startupObject = $assembledStartup
    }
    if (-not (Test-Path -LiteralPath $runtimeArchive)) {
        throw "CVM runtime archive is missing: $runtimeArchive"
    }
    Invoke-Checked -Program $linker -Arguments @(
        $startupObject, $object, $runtimeArchive,
        '-o', $OutputFile,
        '--base', ('0x{0:X}' -f $Base),
        '--entry', $Entry
    ) -Description 'CVM executable link'
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
