$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $projectRoot 'build'

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

& gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 `
    -I (Join-Path $projectRoot 'include') `
    (Join-Path $PSScriptRoot 'main.c') `
    (Join-Path $projectRoot 'src\boot_format.c') `
    -o (Join-Path $buildDir 'vmkimg.exe')
exit $LASTEXITCODE
