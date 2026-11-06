<#
.SYNOPSIS
    Builds the Personal IME Inno Setup Installation Package (x64)
.DESCRIPTION
    Rebuilds native C++ binaries from current source by default (unless -ReuseExistingBinaries),
    publishes the self-contained WPF Desktop Manager, compiles the Inno Setup installer
    with version parameters, and verifies output artifact integrity.
    Does NOT install on the current system.
#>

param(
    [string]$Version = "1.0.0",
    [switch]$ReuseExistingBinaries,
    [switch]$SkipPublishManager,
    [string]$BuildDir = $env:PERSONALIME_BUILD_DIR,
    [string]$OutputDir = $env:PERSONALIME_OUT_DIR
)

$ErrorActionPreference = "Stop"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptRoot

if (-not $BuildDir) { $BuildDir = "build" }
if (-not $OutputDir) { $OutputDir = "out" }

$resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $ProjectRoot $BuildDir }
$resolvedOutDir = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $ProjectRoot $OutputDir }
$installerOutputDir = Join-Path $resolvedOutDir "installer"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Personal IME: Local Installer Build (Inno Setup x64)" -ForegroundColor Cyan
Write-Host "  Target Package Version : $Version" -ForegroundColor Cyan
Write-Host "  Binaries Directory     : $resolvedBuildDir" -ForegroundColor Cyan
Write-Host "  Installer Output       : $installerOutputDir" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Locate ISCC.exe
$isccCandidates = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    "ISCC.exe"
)

$isccPath = $null
foreach ($cand in $isccCandidates) {
    if (Test-Path $cand) {
        $isccPath = $cand
        break
    } else {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if ($cmd) {
            $isccPath = $cmd.Source
            break
        }
    }
}

if (-not $isccPath) {
    throw "Inno Setup Compiler (ISCC.exe) not found. Please install Inno Setup 6."
}
Write-Host "[1/4] Using Inno Setup Compiler: $isccPath" -ForegroundColor Green

# 2. Build or Verify C++ Native Binaries from Current Source
$dll = Join-Path $resolvedBuildDir "Release\PersonalIME.dll"
$service = Join-Path $resolvedBuildDir "Release\ime-service.exe"
$cli = Join-Path $resolvedBuildDir "Release\ime-cli.exe"

if (-not $ReuseExistingBinaries) {
    Write-Host "[2/4] Building C++ native binaries from current source..." -ForegroundColor Cyan
    & (Join-Path $ScriptRoot "build.ps1") -BuildDir $BuildDir
} else {
    Write-Host "[2/4] Reusing existing binaries as requested (-ReuseExistingBinaries)." -ForegroundColor Yellow
}

# Strict post-build verification of all required native artifacts
foreach ($b in @(@{Name="PersonalIME.dll"; Path=$dll}, @{Name="ime-service.exe"; Path=$service}, @{Name="ime-cli.exe"; Path=$cli})) {
    if (-not (Test-Path $b.Path)) {
        throw "Required binary missing: $($b.Name) at $($b.Path)"
    }
}
Write-Host "      Verified C++ binaries (DLL, Service, CLI) in: $resolvedBuildDir\Release" -ForegroundColor Green

# 3. Publish WPF Desktop Manager (Self-Contained)
$managerPublishDir = Join-Path $resolvedOutDir "publish\manager"
$managerExe = Join-Path $managerPublishDir "PersonalIME.Manager.exe"

if (-not $SkipPublishManager -or -not (Test-Path $managerExe)) {
    Write-Host "[3/4] Publishing WPF Desktop Manager (v$Version, Self-Contained x64)..." -ForegroundColor Cyan
    $managerProj = Join-Path $ProjectRoot "src\manager\PersonalIME.Manager\PersonalIME.Manager.csproj"
    dotnet publish $managerProj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:Version=$Version -o $managerPublishDir
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to publish PersonalIME.Manager"
    }
    Write-Host "      WPF Manager published successfully to: $managerPublishDir" -ForegroundColor Green
} else {
    Write-Host "[3/4] Using existing published Manager at: $managerPublishDir" -ForegroundColor Green
}

# 4. Compile Installer via ISCC
Write-Host "[4/4] Compiling installer with ISCC (Version: $Version)..." -ForegroundColor Cyan
if (-not (Test-Path $installerOutputDir)) {
    New-Item -ItemType Directory -Path $installerOutputDir -Force | Out-Null
}

$issPath = Join-Path $ProjectRoot "installer\setup.iss"
& $isccPath "/DMyAppVersion=$Version" $issPath

if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
}

$setupExe = Join-Path $installerOutputDir "PersonalIME-Setup-x64.exe"
if (-not (Test-Path $setupExe)) {
    throw "Expected installer output not found at: $setupExe"
}

$hash = (Get-FileHash -Algorithm SHA256 $setupExe).Hash
$size = (Get-Item $setupExe).Length
$sizeMb = [math]::Round($size / 1MB, 2)

Write-Host "`n==========================================================" -ForegroundColor Green
Write-Host "  Installer Built Successfully!" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
Write-Host "  Package Path : $setupExe" -ForegroundColor Cyan
Write-Host "  Version      : $Version" -ForegroundColor Cyan
Write-Host "  File Size    : $size bytes ($sizeMb MB)" -ForegroundColor Cyan
Write-Host "  SHA256       : $hash" -ForegroundColor Cyan
Write-Host "  [IMPORTANT]  : This installer is generated locally for inspection." -ForegroundColor Yellow
Write-Host "                 DO NOT run or install on current machine without explicit authorization." -ForegroundColor Yellow

[PSCustomObject]@{
    InstallerPath = $setupExe
    Version = $Version
    Size = $size
    SHA256 = $hash
}
