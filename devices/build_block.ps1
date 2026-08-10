$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\devices'
$output = Join-Path $outputDir 'block_device.dll'
$sources = @(
    (Join-Path $PSScriptRoot 'block_device.c'),
    (Join-Path $projectRoot 'src\host_thread.c')
)
$libraries = @()
if (-not ($IsWindows -or $env:OS -eq 'Windows_NT')) {
    $libraries = @('-pthread')
}

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

Push-Location $projectRoot
try {
    & gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
        -shared -I (Join-Path $projectRoot 'include') -I$PSScriptRoot `
        $sources `
        -o $output `
        $libraries
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "Built $output"
