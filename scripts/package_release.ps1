<#
.SYNOPSIS
    Packages Personal IME release artifacts (Portable ZIP & SHA256SUMS)
#>
param(
    [string]$Version = "1.0.0-rc.1",
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

$pkgDir = Join-Path $resolvedOutDir "portable_staging"
$releaseDir = Join-Path $resolvedOutDir "release"

if (Test-Path $pkgDir) { Remove-Item -Path $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $pkgDir "manager") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkgDir "dictionaries") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkgDir "scripts") -Force | Out-Null
if (-not (Test-Path $releaseDir)) { New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null }

Copy-Item (Join-Path $resolvedBuildDir "Release\PersonalIME.dll") -Destination $pkgDir
Copy-Item (Join-Path $resolvedBuildDir "Release\ime-service.exe") -Destination $pkgDir
Copy-Item (Join-Path $resolvedBuildDir "Release\ime-cli.exe") -Destination $pkgDir
Copy-Item (Join-Path $ProjectRoot "LICENSE") -Destination $pkgDir
Copy-Item (Join-Path $ProjectRoot "README.md") -Destination $pkgDir
Copy-Item (Join-Path $ProjectRoot "data\dictionaries\*.dict") -Destination (Join-Path $pkgDir "dictionaries")
Copy-Item (Join-Path $ProjectRoot "data\dictionaries\NOTICE.txt") -Destination (Join-Path $pkgDir "dictionaries")
Copy-Item (Join-Path $ProjectRoot "scripts\register.ps1") -Destination (Join-Path $pkgDir "scripts")
Copy-Item (Join-Path $ProjectRoot "scripts\unregister.ps1") -Destination (Join-Path $pkgDir "scripts")
Copy-Item (Join-Path $resolvedOutDir "publish\manager\*") -Destination (Join-Path $pkgDir "manager") -Recurse

$zipName = "PersonalIME-v$Version-portable-x64.zip"
$zipPath = Join-Path $releaseDir $zipName
if (Test-Path $zipPath) { Remove-Item -Path $zipPath -Force }

Write-Host "Creating Portable ZIP: $zipPath..." -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $pkgDir "*") -DestinationPath $zipPath -Force
Remove-Item -Path $pkgDir -Recurse -Force -ErrorAction SilentlyContinue

$installerSrc = Join-Path $resolvedOutDir "installer\PersonalIME-Setup-x64.exe"
if (Test-Path $installerSrc) {
    Copy-Item $installerSrc -Destination $releaseDir -Force
}

$artifacts = Get-ChildItem -Path $releaseDir -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" }
$sums = @()
Write-Host "`nRelease Artifacts & Checksums:" -ForegroundColor Cyan
foreach ($a in $artifacts) {
    $h = (Get-FileHash -Algorithm SHA256 $a.FullName).Hash
    $line = "$h  $($a.Name)"
    $sums += $line
    Write-Host "  $line ($($a.Length) bytes)" -ForegroundColor Green
}
$sums | Out-File -FilePath (Join-Path $releaseDir "SHA256SUMS.txt") -Encoding ascii

Write-Host "`nRelease packaging completed in: $releaseDir" -ForegroundColor Green

