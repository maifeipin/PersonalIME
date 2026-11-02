$ErrorActionPreference = "Stop"

$cmakePath = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$projectRoot = Split-Path -Parent $PSScriptRoot

Write-Host "==> Configuring CMake (x64 Release with /MT)..." -ForegroundColor Cyan
& $cmakePath -S $projectRoot -B (Join-Path $projectRoot "build") -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

Write-Host "==> Building PersonalIME, ime-service, and test_ipc_runner..." -ForegroundColor Cyan
& $cmakePath --build (Join-Path $projectRoot "build") --config Release
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

Write-Host "==> Build finished successfully!" -ForegroundColor Green
Write-Host "Artifacts located in: $(Join-Path $projectRoot 'build\Release')"
