$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\examples'
$builder = Join-Path $outputDir 'display_demo_builder.exe'
$output = Join-Path $outputDir 'display_demo.bin'

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
    -I (Join-Path $projectRoot 'include') `
    (Join-Path $PSScriptRoot 'display_demo_builder.c') `
    -o $builder
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $builder $output
exit $LASTEXITCODE
