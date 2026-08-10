$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $projectRoot 'build'
$sources = @(
    'tools\assembler\main.c',
    'tools\assembler\assembler.c',
    'tools\assembler\lexer.c',
    'tools\assembler\symbol.c',
    'tools\assembler\encoder.c'
) | ForEach-Object { Join-Path $projectRoot $_ }

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

& gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
    -I (Join-Path $projectRoot 'include') `
    -I$PSScriptRoot `
    $sources `
    -o (Join-Path $buildDir 'vmasm.exe')
exit $LASTEXITCODE
