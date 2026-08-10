$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\examples'
$assembler = Join-Path $projectRoot 'build\vmasm.exe'

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $assembler `
    (Join-Path $PSScriptRoot 'keyboard_demo.asm') `
    -o (Join-Path $outputDir 'keyboard_demo.bin') `
    --symbols (Join-Path $outputDir 'keyboard_demo.sym') `
    --listing (Join-Path $outputDir 'keyboard_demo.lst')
exit $LASTEXITCODE
