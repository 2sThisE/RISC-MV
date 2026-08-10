$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\devices'
$output = Join-Path $outputDir 'display_device.dll'

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

Push-Location $projectRoot
try {
    & gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
        -shared -I (Join-Path $projectRoot 'include') `
        (Join-Path $PSScriptRoot 'display_device.c') `
        -o $output
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "Built $output"
