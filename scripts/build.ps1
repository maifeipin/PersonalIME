param(
    [switch]$RunTests,
    [string]$BuildDir = $env:PERSONALIME_BUILD_DIR
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = "build"
}

# Locate cmake.exe
$cmakePath = "cmake"
$cmakeCmd = Get-Command "cmake" -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
    $vsCmake = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) {
        $cmakePath = $vsCmake
    } else {
        $vsCmake2022 = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $vsCmake2022) {
            $cmakePath = $vsCmake2022
        }
    }
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $projectRoot $BuildDir }

Write-Host "==> Configuring CMake (x64 Release with /MT)..." -ForegroundColor Cyan
Write-Host "    Build Directory: $resolvedBuildDir" -ForegroundColor Cyan
& $cmakePath -S $projectRoot -B $resolvedBuildDir -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

Write-Host "==> Building PersonalIME, ime-service, and native tests..." -ForegroundColor Cyan
& $cmakePath --build $resolvedBuildDir --config Release
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

Write-Host "==> Build finished successfully!" -ForegroundColor Green
Write-Host "Artifacts located in: $(Join-Path $resolvedBuildDir 'Release')"

if ($RunTests) {
    Write-Host "`n==> Running Core & Stage 3 Integration Tests..." -ForegroundColor Cyan
    & (Join-Path $resolvedBuildDir "Release\test_ime_core.exe")
    if ($LASTEXITCODE -ne 0) { throw "test_ime_core failed with exit code $LASTEXITCODE" }
    & (Join-Path $resolvedBuildDir "Release\test_stage3_integration.exe")
    if ($LASTEXITCODE -ne 0) { throw "test_stage3_integration failed with exit code $LASTEXITCODE" }
}
