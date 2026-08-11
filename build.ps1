param(
    [Alias('e')]
    [string[]]$Example,

    [Alias('d')]
    [string[]]$Device,

    [Alias('nc')]
    [switch]$NoClean
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot 'build'
$IsWindowsHost = $IsWindows -or $env:OS -eq 'Windows_NT'
$AllExamples = @(
    'benchmarks',
    'block',
    'boot',
    'calculation',
    'counter',
    'display',
    'hello',
    'keyboard',
    'linker',
    'llvm_ir',
    'syscall'
)
$AllDevices = @('block', 'display', 'keyboard', 'sample_counter')

function Assert-LastExitCode([string]$Action) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Action failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Gcc([string[]]$CompilerArguments, [string]$Action) {
    & gcc @CompilerArguments
    Assert-LastExitCode $Action
}

function Get-LlvmDevelopmentPaths {
    $llvmConfig = Get-Command llvm-config -ErrorAction SilentlyContinue
    if ($null -eq $llvmConfig) {
        throw 'LLVM development tools are required: llvm-config was not found in PATH'
    }
    $version = (& $llvmConfig.Source --version).Trim()
    Assert-LastExitCode 'LLVM version query'
    if (-not $version.StartsWith('22.')) {
        throw "LLVM 22.x is required, but llvm-config reports $version"
    }
    $prefix = (& $llvmConfig.Source --prefix).Trim()
    Assert-LastExitCode 'LLVM prefix query'
    $include = Join-Path $prefix 'include'
    $library = Join-Path $prefix 'lib'
    if (-not (Test-Path -LiteralPath (Join-Path $include 'llvm-c\IRReader.h')) -or
        -not (Test-Path -LiteralPath $library)) {
        throw "LLVM 22 development headers or libraries are missing under $prefix"
    }
    return [pscustomobject]@{
        Version = $version
        Include = $include
        Library = $library
    }
}

function New-BuildDirectory([string]$RelativePath) {
    $path = Join-Path $BuildRoot $RelativePath
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

function Resolve-Selection(
    [string[]]$Requested,
    [string[]]$Available,
    [string]$Kind,
    [bool]$BuildAll
) {
    if ($BuildAll) {
        return @($Available)
    }
    if ($null -eq $Requested) {
        return @()
    }
    $resolved = @()
    foreach ($name in $Requested) {
        if ($name -eq 'all') {
            $resolved += $Available
            continue
        }
        if ($Available -notcontains $name) {
            $valid = $Available -join ', '
            throw "Unknown $Kind project '$name'. Valid names: $valid"
        }
        $resolved += $name
    }
    return @($resolved | Select-Object -Unique)
}

function Build-Main {
    Write-Host '[main] building VM executable'
    $sources = @(
        'src\main.c',
        'src\main_options.c',
        'src\boot_format.c',
        'src\bus.c',
        'src\core_control.c',
        'src\cpu.c',
        'src\cpu_vector.c',
        'src\device_manager.c',
        'src\display_queue.c',
        'src\headless_display.c',
        'src\host_thread.c',
        'src\interrupt.c',
        'src\irq_controller.c',
        'src\keyboard_input.c',
        'src\mmu.c',
        'src\module_loader.c',
        'src\ram.c',
        'src\system_control.c',
        'src\system_info.c',
        'src\timer.c',
        'src\uart.c',
        'src\vm.c',
        'src\vm_clock.c',
        'src\window_display.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include')
    ) + $sources + @('-o', (Join-Path $BuildRoot 'main.exe'))
    if ($IsWindowsHost) {
        $arguments += @('-luser32', '-lgdi32')
    }
    Invoke-Gcc $arguments 'main build'
}

function Build-Assembler {
    Write-Host '[tools/assembler] building vmasm'
    $output = New-BuildDirectory 'tools'
    $sources = @(
        'tools\assembler\main.c',
        'tools\assembler\assembler.c',
        'tools\assembler\lexer.c',
        'tools\assembler\symbol.c',
        'tools\assembler\encoder.c',
        'tools\assembler\object_assembler.c',
        'tools\assembler\preprocessor.c',
        'tools\object\object_format.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', (Join-Path $ProjectRoot 'tools\assembler'),
        '-I', (Join-Path $ProjectRoot 'tools\object')
    ) + $sources + @('-o', (Join-Path $output 'vmasm.exe'))
    Invoke-Gcc $arguments 'assembler build'
}

function Build-Linker {
    Write-Host '[tools/linker] building cvmlink'
    $output = New-BuildDirectory 'tools'
    $sources = @(
        'tools\linker\main.c',
        'tools\object\archive_format.c',
        'tools\object\object_format.c',
        'src\boot_format.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', (Join-Path $ProjectRoot 'tools\object')
    ) + $sources + @('-o', (Join-Path $output 'cvmlink.exe'))
    Invoke-Gcc $arguments 'linker build'
}

function Build-ArchiveTool {
    Write-Host '[tools/archive] building cvmar'
    $output = New-BuildDirectory 'tools'
    $sources = @(
        'tools\archive\main.c',
        'tools\object\archive_format.c',
        'tools\object\object_format.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'tools\object')
    ) + $sources + @('-o', (Join-Path $output 'cvmar.exe'))
    Invoke-Gcc $arguments 'archive tool build'
}

function Build-CCompiler {
    Write-Host '[tools/compiler] building cvmcc'
    $output = New-BuildDirectory 'tools'
    $sources = @(
        'tools\compiler\main.c',
        'tools\compiler\compiler.c',
        'tools\assembler\assembler.c',
        'tools\assembler\lexer.c',
        'tools\assembler\symbol.c',
        'tools\assembler\encoder.c',
        'tools\assembler\object_assembler.c',
        'tools\object\object_format.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', (Join-Path $ProjectRoot 'tools\compiler'),
        '-I', (Join-Path $ProjectRoot 'tools\assembler'),
        '-I', (Join-Path $ProjectRoot 'tools\object')
    ) + $sources + @('-o', (Join-Path $output 'cvmcc.exe'))
    Invoke-Gcc $arguments 'C compiler build'
}

function Build-IrTranslator {
    $llvm = Get-LlvmDevelopmentPaths
    Write-Host "[tools/ir_translator] building cvmir with LLVM $($llvm.Version)"
    $output = New-BuildDirectory 'tools'
    $sources = @(
        'tools\ir_translator\main.c',
        'tools\ir_translator\cvmir_llvm.c',
        'tools\assembler\assembler.c',
        'tools\assembler\lexer.c',
        'tools\assembler\symbol.c',
        'tools\assembler\encoder.c',
        'tools\assembler\object_assembler.c',
        'tools\object\object_format.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', (Join-Path $ProjectRoot 'tools\ir_translator'),
        '-I', (Join-Path $ProjectRoot 'tools\assembler'),
        '-I', (Join-Path $ProjectRoot 'tools\object'),
        '-I', $llvm.Include
    ) + $sources + @(
        '-L', $llvm.Library,
        '-lLLVM-22',
        '-o', (Join-Path $output 'cvmir.exe')
    )
    Invoke-Gcc $arguments 'LLVM IR translator build'

    $driver = Join-Path $output 'cvmclang.ps1'
    Copy-Item -LiteralPath (Join-Path $ProjectRoot `
        'tools\ir_translator\cvmclang.ps1') -Destination $driver -Force
    New-BuildDirectory 'sysroot' | Out-Null
    $sysrootInclude = New-BuildDirectory 'sysroot\include'
    $sysrootLibrary = New-BuildDirectory 'sysroot\lib'
    Copy-Item -Path (Join-Path $ProjectRoot `
        'tools\ir_translator\sysroot\include\*') `
        -Destination $sysrootInclude -Recurse -Force
    $builtinsObject = Join-Path $sysrootLibrary 'builtins.o'
    & $driver (Join-Path $ProjectRoot `
        'tools\ir_translator\sysroot\builtins.c') `
        -CompileOnly -OutputFile $builtinsObject
    Assert-LastExitCode 'CVM runtime builtins compilation'
    & (Join-Path $output 'vmasm.exe') `
        (Join-Path $ProjectRoot 'tools\ir_translator\sysroot\crt0.s') `
        -c -o (Join-Path $sysrootLibrary 'crt0.o')
    Assert-LastExitCode 'CVM crt0 assembly'
    & (Join-Path $output 'cvmar.exe') rcs `
        (Join-Path $sysrootLibrary 'libcvm.a') $builtinsObject
    Assert-LastExitCode 'CVM runtime archive creation'
}

function Build-KernelImageTool {
    Write-Host '[tools/kernel_image] building vmkimg'
    $output = New-BuildDirectory 'tools'
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        (Join-Path $ProjectRoot 'tools\kernel_image\main.c'),
        (Join-Path $ProjectRoot 'src\boot_format.c'),
        '-o', (Join-Path $output 'vmkimg.exe')
    )
    Invoke-Gcc $arguments 'kernel image tool build'
}

function Build-DiskImageTool {
    Write-Host '[tools/disk_image] building vmkdisk'
    $output = New-BuildDirectory 'tools'
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        (Join-Path $ProjectRoot 'tools\disk_image\main.c'),
        (Join-Path $ProjectRoot 'src\disk_image.c'),
        (Join-Path $ProjectRoot 'src\boot_format.c'),
        '-o', (Join-Path $output 'vmkdisk.exe')
    )
    if ($IsWindowsHost) {
        $arguments += '-lbcrypt'
    }
    Invoke-Gcc $arguments 'disk image tool build'
}

function Build-AllTools {
    Build-Assembler
    Build-Linker
    Build-ArchiveTool
    Build-IrTranslator
    Build-KernelImageTool
    Build-DiskImageTool
}

function Build-CompilerExample {
    $output = New-BuildDirectory 'examples'
    $symbolOutput = New-BuildDirectory 'examples\sym'
    $compiler = Join-Path $BuildRoot 'tools\cvmcc.exe'
    $assembler = Join-Path $BuildRoot 'tools\vmasm.exe'
    $linker = Join-Path $BuildRoot 'tools\cvmlink.exe'
    & $compiler -c (Join-Path $ProjectRoot 'examples\compiler\compiler_demo.c') `
        -o (Join-Path $output 'compiler_demo.o')
    Assert-LastExitCode 'compiler example C compilation'
    & $assembler (Join-Path $ProjectRoot 'examples\compiler\start.s') -c `
        -o (Join-Path $output 'compiler_start.o')
    Assert-LastExitCode 'compiler example startup assembly'
    & $linker (Join-Path $output 'compiler_start.o') `
        (Join-Path $output 'compiler_demo.o') `
        -o (Join-Path $output 'compiler_demo.cvm') `
        --base 0x10000 --entry compiler_demo_entry `
        --map (Join-Path $symbolOutput 'compiler_demo.map')
    Assert-LastExitCode 'compiler example link'
}

function Build-LLVMIRExample {
    $output = New-BuildDirectory 'examples'
    $symbolOutput = New-BuildDirectory 'examples\sym'
    $translator = Join-Path $BuildRoot 'tools\cvmir.exe'
    $clangDriver = Join-Path $BuildRoot 'tools\cvmclang.ps1'
    $assembler = Join-Path $BuildRoot 'tools\vmasm.exe'
    $linker = Join-Path $BuildRoot 'tools\cvmlink.exe'
    & $translator -c (Join-Path $ProjectRoot 'examples\llvm_ir\arithmetic.ll') `
        -o (Join-Path $output 'llvm_arithmetic.o')
    Assert-LastExitCode 'LLVM IR example translation'
    & $assembler (Join-Path $ProjectRoot 'examples\llvm_ir\start.s') -c `
        -o (Join-Path $output 'llvm_start.o')
    Assert-LastExitCode 'LLVM IR example startup assembly'
    & $linker (Join-Path $output 'llvm_start.o') `
        (Join-Path $output 'llvm_arithmetic.o') `
        -o (Join-Path $output 'llvm_arithmetic.cvm') `
        --base 0x10000 --entry cvmir_demo_entry `
        --map (Join-Path $symbolOutput 'llvm_arithmetic.map')
    Assert-LastExitCode 'LLVM IR example link'

    $clang = Get-Command clang -ErrorAction SilentlyContinue
    if ($null -ne $clang) {
        Write-Host '[examples/llvm_ir] validating cvmclang C pipeline'
        & $clangDriver `
            (Join-Path $ProjectRoot 'examples\llvm_ir\clang_arithmetic.c') `
            -CompileOnly `
            -o (Join-Path $output 'clang_arithmetic.o')
        Assert-LastExitCode 'cvmclang arithmetic compilation'
        & $linker (Join-Path $output 'llvm_start.o') `
            (Join-Path $output 'clang_arithmetic.o') `
            -o (Join-Path $output 'clang_arithmetic.cvm') `
            --base 0x10000 --entry cvmir_demo_entry `
            --map (Join-Path $symbolOutput 'clang_arithmetic.map')
        Assert-LastExitCode 'installed Clang IR link'

        & $clangDriver `
            (Join-Path $ProjectRoot 'examples\llvm_ir\kernel_memory.c') `
            -o (Join-Path $output 'kernel_memory.cvm') `
            -Startup (Join-Path $ProjectRoot `
                'examples\llvm_ir\kernel_memory_start.s') `
            -Entry cvm_kernel_memory_entry `
            -Base 0x10000
        Assert-LastExitCode 'cvmclang memory kernel compilation and link'

        & $clangDriver `
            (Join-Path $ProjectRoot 'examples\llvm_ir\freestanding_main.c') `
            -o (Join-Path $output 'freestanding_main.cvm')
        Assert-LastExitCode 'cvmclang default crt0 and libcvm link'
    } else {
        Write-Host '[examples/llvm_ir] Clang not found; skipping host IR check'
    }
}

function Build-Kernel {
    Write-Host '[kernel] building C reference kernel with assembly boundary'
    $output = New-BuildDirectory 'kernel'
    $assembler = Join-Path $BuildRoot 'tools\vmasm.exe'
    $clangDriver = Join-Path $BuildRoot 'tools\cvmclang.ps1'
    $linker = Join-Path $BuildRoot 'tools\cvmlink.exe'
    & $assembler (Join-Path $ProjectRoot 'kernel\layout_start.s') -c `
        -o (Join-Path $output 'layout_start.o')
    Assert-LastExitCode 'kernel layout start assembly'
    & $assembler (Join-Path $ProjectRoot 'kernel\kernel.s') -c `
        -o (Join-Path $output 'kernel.o')
    Assert-LastExitCode 'kernel assembly'
    $kernelCObjects = @()
    foreach ($sourceName in @('kernel_main.c', 'pmm.c', 'mmu.c', 'heap.c',
                               'runtime.c', 'address_space.c', 'user_loader.c',
                               'devices.c', 'fat32.c', 'vfs.c',
                               'syscall.c', 'scheduler.c', 'exception.c')) {
        $objectName = [System.IO.Path]::ChangeExtension($sourceName, '.o')
        $objectPath = Join-Path $output $objectName
        & $clangDriver (Join-Path $ProjectRoot "kernel\$sourceName") `
            -CompileOnly -IncludeDirectory (Join-Path $ProjectRoot 'include') `
            -OutputFile $objectPath
        Assert-LastExitCode "kernel C compilation ($sourceName)"
        $kernelCObjects += $objectPath
    }
    & $assembler (Join-Path $ProjectRoot 'kernel\layout_end.s') -c `
        -o (Join-Path $output 'layout_end.o')
    Assert-LastExitCode 'kernel layout end assembly'
    $kernelMap = Join-Path (New-BuildDirectory 'kernel\sym') 'kernel.map'
    $kernelLinkArguments = @(
        (Join-Path $output 'layout_start.o'),
        (Join-Path $output 'kernel.o')
    ) + $kernelCObjects + @(
        (Join-Path $output 'layout_end.o'),
        (Join-Path $BuildRoot 'sysroot\lib\libcvm.a'),
        '-o', (Join-Path $output 'kernel.cvm'),
        '--base', '0x40000000', '--entry', 'kernel_entry',
        '--physical-relocatable',
        '--map', $kernelMap
    )
    & $linker @kernelLinkArguments
    Assert-LastExitCode 'kernel link'

    $kernelEndLine = Select-String -LiteralPath $kernelMap `
        -Pattern '^([0-9A-Fa-f]{16}) kernel_bss_end$'
    if ($null -eq $kernelEndLine) {
        throw 'kernel link: kernel_bss_end is missing from the map file'
    }
    $kernelEnd = [Convert]::ToUInt64($kernelEndLine.Matches[0].Groups[1].Value, 16)
    $kernelImageSize = [uint64](Get-Item -LiteralPath `
        (Join-Path $output 'kernel.cvm')).Length
    $minimumStagingSize = [uint64]([Math]::Ceiling(
        $kernelImageSize / 4096.0) * 4096)
    if ($minimumStagingSize -gt 0xD0000) {
        throw ('kernel link: page-rounded image file size 0x{0:X} leaves no staging space above kernel base 0x30000 in the 1 MiB minimum RAM profile' -f `
            $minimumStagingSize)
    }
    $kernelSpan = $kernelEnd - [uint64]0x40000000
    $minimumStagingStart = [uint64](0x100000 - $minimumStagingSize)
    $kernelPhysicalSpan = [uint64]([Math]::Ceiling(
        $kernelSpan / 4096.0) * 4096)
    $kernelLevel0Tables = [uint64][Math]::Ceiling(
        $kernelSpan / 2097152.0)
    # root + low identity (2) + kernel L1/L0 + direct-map L1/L0 + UART L0
    $minimumPageTableSize = [uint64](7 + $kernelLevel0Tables) * 4096
    $minimumPageTableEnd = [uint64]0x30000 + $kernelPhysicalSpan +
                           $minimumPageTableSize
    if ($minimumPageTableEnd -gt $minimumStagingStart) {
        throw ('kernel link: relocated image span 0x{0:X} plus initial page tables 0x{1:X} exceed minimum-RAM staging boundary 0x{2:X}' -f `
            $kernelSpan, $minimumPageTableSize, $minimumStagingStart)
    }
}

function Build-DeviceProject([string]$Name) {
    Write-Host "[devices/$Name] building module"
    $output = New-BuildDirectory 'devices'
    $sourceDirectory = Join-Path $ProjectRoot "devices\$Name"
    $sourceName = switch ($Name) {
        'block' { 'block_device.c' }
        'display' { 'display_device.c' }
        'keyboard' { 'keyboard_device.c' }
        'sample_counter' { 'sample_counter.c' }
    }
    $binaryName = switch ($Name) {
        'block' { 'block_device.dll' }
        'display' { 'display_device.dll' }
        'keyboard' { 'keyboard_device.dll' }
        'sample_counter' { 'sample_counter.dll' }
    }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-shared',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', $sourceDirectory,
        (Join-Path $sourceDirectory $sourceName)
    )
    if ($Name -eq 'block') {
        $arguments += (Join-Path $ProjectRoot 'src\host_thread.c')
    }
    $arguments += @('-o', (Join-Path $output $binaryName))
    if (-not $IsWindowsHost -and $Name -eq 'block') {
        $arguments += '-pthread'
    }
    Invoke-Gcc $arguments "device $Name build"
    Copy-Item -LiteralPath (Join-Path $output $binaryName) `
        -Destination (Join-Path $BuildRoot "modules\$binaryName") -Force
    if ($Name -eq 'block') {
        $fallbackDisk = Join-Path $output 'block_device.img'
        $configuration = "path=$fallbackDisk;create=67108864"
        Set-Content -LiteralPath (Join-Path $BuildRoot 'modules\block_device.conf') `
            -Value $configuration -NoNewline -Encoding Ascii
    }
}

function Build-RawAssemblyExample(
    [string]$Project,
    [string]$SourceName,
    [string]$OutputName,
    [string]$Base = '1'
) {
    $output = New-BuildDirectory 'examples'
    $listingOutput = New-BuildDirectory 'examples\lst'
    $symbolOutput = New-BuildDirectory 'examples\sym'
    $assembler = Join-Path $BuildRoot 'tools\vmasm.exe'
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($OutputName)
    & $assembler (Join-Path $ProjectRoot "examples\$Project\$SourceName") `
        -o (Join-Path $output $OutputName) --base $Base `
        --symbols (Join-Path $symbolOutput "$stem.sym") `
        --listing (Join-Path $listingOutput "$stem.lst")
    Assert-LastExitCode "example $Project assembly"
}

function Build-BenchmarksExample {
    foreach ($name in @('bench_alu', 'bench_branch', 'bench_loop',
                         'bench_memory', 'bench_mmu')) {
        Build-RawAssemblyExample 'benchmarks' "$name.asm" "$name.bin"
    }
}

function Build-DisplayExample {
    $output = New-BuildDirectory 'examples'
    $builder = Join-Path (New-BuildDirectory 'tools') 'display_demo_builder.exe'
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', (Join-Path $ProjectRoot 'examples\display'),
        (Join-Path $ProjectRoot 'examples\display\display_demo_builder.c'),
        '-o', $builder
    )
    Invoke-Gcc $arguments 'display example builder'
    & $builder (Join-Path $output 'display_demo.bin')
    Assert-LastExitCode 'display example generation'
}

function Build-LinkerExample {
    $output = New-BuildDirectory 'examples'
    $symbolOutput = New-BuildDirectory 'examples\sym'
    $assembler = Join-Path $BuildRoot 'tools\vmasm.exe'
    $linker = Join-Path $BuildRoot 'tools\cvmlink.exe'
    $archiver = Join-Path $BuildRoot 'tools\cvmar.exe'
    & $assembler (Join-Path $ProjectRoot 'examples\linker\linker_demo_main.s') `
        -c -o (Join-Path $output 'linker_demo_main.o')
    Assert-LastExitCode 'linker example main assembly'
    & $assembler (Join-Path $ProjectRoot 'examples\linker\linker_demo_support.s') `
        -c -o (Join-Path $output 'linker_demo_support.o')
    Assert-LastExitCode 'linker example support assembly'
    & $assembler (Join-Path $ProjectRoot 'examples\linker\linker_demo_optional.s') `
        -c -o (Join-Path $output 'linker_demo_optional.o')
    Assert-LastExitCode 'linker example optional assembly'
    & $archiver create (Join-Path $output 'liblinker_demo.a') `
        (Join-Path $output 'linker_demo_support.o') `
        (Join-Path $output 'linker_demo_optional.o')
    Assert-LastExitCode 'linker example archive creation'
    & $linker (Join-Path $output 'linker_demo_main.o') `
        (Join-Path $output 'liblinker_demo.a') `
        -o (Join-Path $output 'linker_demo.cvm') `
        --map (Join-Path $symbolOutput 'linker_demo.map')
    Assert-LastExitCode 'linker example link'
}

function Build-BootExample {
    $output = New-BuildDirectory 'examples'
    $listingOutput = New-BuildDirectory 'examples\lst'
    $symbolOutput = New-BuildDirectory 'examples\sym'
    $assembler = Join-Path $BuildRoot 'tools\vmasm.exe'
    $imageTool = Join-Path $BuildRoot 'tools\vmkimg.exe'
    $diskTool = Join-Path $BuildRoot 'tools\vmkdisk.exe'

    & $assembler (Join-Path $ProjectRoot 'examples\boot\boot_rom.asm') `
        -o (Join-Path $output 'boot_rom.bin') --base 0x7FFFF00000 `
        --symbols (Join-Path $symbolOutput 'boot_rom.sym') `
        --listing (Join-Path $listingOutput 'boot_rom.lst')
    Assert-LastExitCode 'boot ROM assembly'

    & $assembler (Join-Path $ProjectRoot 'examples\boot\bootloader.asm') `
        -o (Join-Path $output 'bootloader.bin') --base 0x20000 `
        --symbols (Join-Path $symbolOutput 'bootloader.sym')
    Assert-LastExitCode 'bootloader assembly'
    & $imageTool pack (Join-Path $output 'bootloader.bin') `
        -o (Join-Path $output 'bootloader.cvm') `
        --load 0x20000 --entry 0x20000 --memory-size 0x4000
    Assert-LastExitCode 'bootloader packaging'

    & $assembler (Join-Path $ProjectRoot 'examples\boot\kernel_stub.asm') `
        -o (Join-Path $output 'kernel_stub.bin') --base 0x30000 `
        --symbols (Join-Path $symbolOutput 'kernel_stub.sym')
    Assert-LastExitCode 'kernel stub assembly'
    & $imageTool pack (Join-Path $output 'kernel_stub.bin') `
        -o (Join-Path $output 'kernel_stub.cvm') `
        --load 0x30000 --entry 0x30000 --memory-size 0x1000
    Assert-LastExitCode 'kernel stub packaging'

    $diskPath = Join-Path $output 'system.img'
    if (Test-Path -LiteralPath $diskPath) {
        Remove-Item -LiteralPath $diskPath -Force
    }
    & $diskTool create -o $diskPath --size 64M `
        --bootloader (Join-Path $output 'bootloader.cvm') `
        --kernel (Join-Path $BuildRoot 'kernel\kernel.cvm') `
        --reproducible
    Assert-LastExitCode 'boot disk generation'
    if (Test-Path -LiteralPath (Join-Path $BuildRoot 'modules\block_device.dll')) {
        $configuration = "path=$diskPath;readonly=false"
        Set-Content -LiteralPath (Join-Path $BuildRoot 'modules\block_device.conf') `
            -Value $configuration -NoNewline -Encoding Ascii
    }
}

function Build-ExampleProject([string]$Name) {
    Write-Host "[examples/$Name] building project"
    switch ($Name) {
        'benchmarks' { Build-BenchmarksExample }
        'block' { Build-RawAssemblyExample 'block' 'block_demo.asm' 'block_demo.bin' }
        'boot' { Build-BootExample }
        'calculation' {
            $output = New-BuildDirectory 'examples'
            Copy-Item -LiteralPath (Join-Path $ProjectRoot 'examples\calculation\calculation.bin') `
                -Destination (Join-Path $output 'calculation.bin') -Force
        }
        'compiler' { Build-CompilerExample }
        'counter' { Build-RawAssemblyExample 'counter' 'counter.asm' 'counter.bin' }
        'display' { Build-DisplayExample }
        'hello' { Build-RawAssemblyExample 'hello' 'hello.asm' 'hello.bin' }
        'keyboard' { Build-RawAssemblyExample 'keyboard' 'keyboard_demo.asm' 'keyboard_demo.bin' }
        'linker' { Build-LinkerExample }
        'llvm_ir' { Build-LLVMIRExample }
        'syscall' { Build-RawAssemblyExample 'syscall' 'syscall_demo.asm' 'syscall_demo.bin' }
    }
}

function Build-Tests {
    Write-Host '[tests] building test runner'
    $llvm = Get-LlvmDevelopmentPaths
    $output = New-BuildDirectory 'tools'
    $testSources = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'tests') `
        -Filter '*_test.c' | Sort-Object Name | ForEach-Object FullName
    $productionSources = @(
        'src\boot_format.c', 'src\bus.c', 'src\core_control.c',
        'src\cpu.c', 'src\cpu_vector.c', 'src\device_manager.c',
        'src\disk_image.c', 'src\display_queue.c', 'src\host_thread.c',
        'src\headless_display.c', 'src\interrupt.c',
        'src\irq_controller.c', 'src\keyboard_input.c',
        'src\main_options.c', 'src\mmu.c', 'src\module_loader.c',
        'src\ram.c', 'src\system_control.c', 'src\system_info.c',
        'src\timer.c', 'src\uart.c', 'src\vm.c', 'src\vm_clock.c',
        'tools\assembler\assembler.c', 'tools\assembler\lexer.c',
        'tools\assembler\symbol.c', 'tools\assembler\encoder.c',
        'tools\assembler\object_assembler.c',
        'tools\compiler\compiler.c',
        'tools\ir_translator\cvmir_llvm.c',
        'tools\object\archive_format.c',
        'tools\object\object_format.c'
    ) | ForEach-Object { Join-Path $ProjectRoot $_ }
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O2',
        '-DVM_BLOCK_DEVICE_STATIC', '-DVM_KEYBOARD_DEVICE_STATIC',
        '-I', (Join-Path $ProjectRoot 'include'),
        '-I', (Join-Path $ProjectRoot 'tools\assembler'),
        '-I', (Join-Path $ProjectRoot 'tools\compiler'),
        '-I', (Join-Path $ProjectRoot 'tools\ir_translator'),
        '-I', (Join-Path $ProjectRoot 'tools\object'),
        '-I', $llvm.Include,
        '-I', (Join-Path $ProjectRoot 'devices\block'),
        '-I', (Join-Path $ProjectRoot 'devices\display'),
        '-I', (Join-Path $ProjectRoot 'devices\keyboard'),
        (Join-Path $ProjectRoot 'tests\test_runner.c')
    ) + $testSources + @(
        (Join-Path $ProjectRoot 'devices\block\block_device.c'),
        (Join-Path $ProjectRoot 'devices\display\display_device.c'),
        (Join-Path $ProjectRoot 'devices\keyboard\keyboard_device.c')
    ) + $productionSources + @(
        '-L', $llvm.Library,
        '-lLLVM-22',
        '-o', (Join-Path $output 'test_runner.exe')
    )
    if ($IsWindowsHost) {
        $arguments += '-lbcrypt'
    }
    Invoke-Gcc $arguments 'test runner build'
}

$Selective = $PSBoundParameters.ContainsKey('Example') -or
             $PSBoundParameters.ContainsKey('Device')
$BuildAll = -not $Selective
$SelectedExamples = Resolve-Selection $Example $AllExamples 'example' $BuildAll
$SelectedDevices = Resolve-Selection $Device $AllDevices 'device' $BuildAll

if (-not $NoClean -and (Test-Path -LiteralPath $BuildRoot)) {
    $resolvedRoot = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
    $resolvedBuild = [System.IO.Path]::GetFullPath($BuildRoot).TrimEnd('\', '/')
    if (-not $resolvedBuild.StartsWith($resolvedRoot +
            [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean build directory outside project: $resolvedBuild"
    }
    Write-Host "[clean] removing $resolvedBuild"
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
New-BuildDirectory 'modules' | Out-Null
foreach ($type in @('examples', 'devices', 'tools')) {
    New-BuildDirectory $type | Out-Null
    New-BuildDirectory "$type\lst" | Out-Null
    New-BuildDirectory "$type\sym" | Out-Null
}

if ($BuildAll -or $SelectedExamples.Count -ne 0) {
    Build-Main
    Build-AllTools
}
if ($BuildAll -or $SelectedExamples -contains 'boot') {
    Build-Kernel
}
foreach ($name in $SelectedDevices) {
    Build-DeviceProject $name
}
foreach ($name in $SelectedExamples) {
    Build-ExampleProject $name
}
if ($BuildAll) {
    Build-Tests
}

Write-Host ''
Write-Host "Build completed: $BuildRoot"
