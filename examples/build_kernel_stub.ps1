$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build\examples'

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

& (Join-Path $projectRoot 'tools\assembler\build.ps1')
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $projectRoot 'tools\kernel_image\build.ps1')
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $projectRoot 'build\vmasm.exe') `
    (Join-Path $PSScriptRoot 'kernel_stub.asm') `
    -o (Join-Path $buildDir 'kernel_stub.bin') `
    --base 0x10000 `
    --symbols (Join-Path $buildDir 'kernel_stub.sym')
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& (Join-Path $projectRoot 'build\vmkimg.exe') pack `
    (Join-Path $buildDir 'kernel_stub.bin') `
    -o (Join-Path $buildDir 'kernel_stub.cvm') `
    --load 0x10000 `
    --entry 0x10000 `
    --memory-size 0x1000
exit $LASTEXITCODE
