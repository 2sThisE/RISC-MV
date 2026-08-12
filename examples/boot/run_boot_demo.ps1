$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$romPath = Join-Path $projectRoot 'build\examples\boot_rom.bin'
$mainPath = Join-Path $projectRoot 'build\main.exe'

& (Join-Path $projectRoot 'build.ps1') -e boot -d block -nc
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $mainPath `
    -r 2097152 `
    -rom $romPath
exit $LASTEXITCODE
