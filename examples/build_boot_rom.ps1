$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\examples'
$assembler = Join-Path $projectRoot 'build\vmasm.exe'

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $assembler `
    (Join-Path $PSScriptRoot 'boot_rom.asm') `
    -o (Join-Path $outputDir 'boot_rom.bin') `
    --base 0x7FFFF00000 `
    --symbols (Join-Path $outputDir 'boot_rom.sym') `
    --listing (Join-Path $outputDir 'boot_rom.lst')
exit $LASTEXITCODE
