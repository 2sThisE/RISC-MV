$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
$moduleDir = Join-Path $buildDir 'modules'
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
) | ForEach-Object { Join-Path $projectRoot $_ }

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
New-Item -ItemType Directory -Path $moduleDir -Force | Out-Null

$libraries = @()
if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    $libraries = @('-luser32', '-lgdi32')
}

& gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
    -I (Join-Path $projectRoot 'include') `
    $sources `
    -o (Join-Path $buildDir 'main.exe') `
    $libraries
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
    -I (Join-Path $projectRoot 'include') `
    (Join-Path $projectRoot 'tools\kernel_image\main.c') `
    (Join-Path $projectRoot 'src\boot_format.c') `
    -o (Join-Path $buildDir 'vmkimg.exe')
exit $LASTEXITCODE
