$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
$testSources = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*_test.c' |
    Sort-Object Name |
    ForEach-Object FullName
$productionSources = @(
    'src\boot_format.c',
    'src\bus.c',
    'src\core_control.c',
    'src\cpu.c',
    'src\cpu_vector.c',
    'src\device_manager.c',
    'src\display_queue.c',
    'src\host_thread.c',
    'src\headless_display.c',
    'src\interrupt.c',
    'src\irq_controller.c',
    'src\keyboard_input.c',
    'src\main_options.c',
    'src\mmu.c',
    'src\module_loader.c',
    'src\ram.c',
    'src\system_control.c',
    'src\system_info.c',
    'src\timer.c',
    'src\uart.c',
    'src\vm.c',
    'src\vm_clock.c',
    'tools\assembler\assembler.c',
    'tools\assembler\lexer.c',
    'tools\assembler\symbol.c',
    'tools\assembler\encoder.c'
) | ForEach-Object { Join-Path $projectRoot $_ }

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

Push-Location $projectRoot
try {
    & gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
        -DVM_BLOCK_DEVICE_STATIC `
        -DVM_KEYBOARD_DEVICE_STATIC `
        -I (Join-Path $projectRoot 'include') `
        -I (Join-Path $projectRoot 'tools\assembler') `
        -I (Join-Path $projectRoot 'devices') `
        (Join-Path $PSScriptRoot 'test_runner.c') `
        $testSources `
        (Join-Path $projectRoot 'devices\block_device.c') `
        (Join-Path $projectRoot 'devices\display_device.c') `
        (Join-Path $projectRoot 'devices\keyboard_device.c') `
        $productionSources `
        -o (Join-Path $buildDir 'test_runner.exe')
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host ("Built " + (Join-Path $buildDir 'test_runner.exe'))
