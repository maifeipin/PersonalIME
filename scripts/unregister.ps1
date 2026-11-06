<#
.SYNOPSIS
    Unregisters Personal IME TSF In-Proc Server DLL (x64)
.DESCRIPTION
    Safely unregisters the 64-bit PersonalIME.dll from Windows COM & TSF Category Manager.
    Includes architecture verification, explicit 64-bit regsvr32 path, and post-check.
#>
param(
    [string]$DllPath = "",
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# Architecture verification
if (-not [Environment]::Is64BitOperatingSystem) {
    throw "Personal IME requires a 64-bit Windows operating system."
}

# Resolve DLL Path: supports portable release layout, same-dir layout, and development layout
if (-not $DllPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\PersonalIME.dll"),              # Portable layout: root DLL, script in scripts/
        (Join-Path $PSScriptRoot "PersonalIME.dll"),                 # Sibling layout
        (Join-Path $PSScriptRoot "..\build\Release\PersonalIME.dll") # Development build layout
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) {
            $DllPath = $cand
            break
        }
    }
    if (-not $DllPath) {
        $DllPath = Join-Path $PSScriptRoot "..\build\Release\PersonalIME.dll"
    }
}

$resolvedDll = if (Test-Path $DllPath) { (Resolve-Path $DllPath).Path } else { $DllPath }

if ($DryRun) {
    Write-Host "[DRY-RUN] PersonalIME TSF DLL unregistration check:" -ForegroundColor Cyan
    Write-Host "  Target DLL: $resolvedDll" -ForegroundColor Gray
    Write-Host "  Architecture: x64" -ForegroundColor Gray
    Write-Host "  RegSvr32: $env:windir\System32\regsvr32.exe /u" -ForegroundColor Gray
    Write-Host "  CLSID: {7A5074A1-4F29-4FBE-A2D0-244D42B28B01}" -ForegroundColor Gray
    return
}

# Elevate privilege check
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Unregistering a TSF input method requires Administrator privileges. Relaunching as Admin..."
    $argString = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -DllPath `"$resolvedDll`""
    $elevated = Start-Process powershell.exe -ArgumentList $argString -Verb RunAs -WindowStyle Normal -Wait -PassThru
    exit $elevated.ExitCode
}

Write-Host "Unregistering TSF DLL (x64): $resolvedDll" -ForegroundColor Cyan

# Explicit 64-bit regsvr32
$regsvr64 = Join-Path $env:windir "System32\regsvr32.exe"
if (-not (Test-Path $regsvr64)) {
    $regsvr64 = "regsvr32.exe"
}

$proc = Start-Process -FilePath $regsvr64 -ArgumentList "/u /s `"$resolvedDll`"" -PassThru -Wait
if ($proc.ExitCode -ne 0) {
    throw "regsvr32 /u failed with exit code: $($proc.ExitCode)"
}

# Post-check registry key
$clsidKey = "Registry::HKEY_CLASSES_ROOT\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}"
if (-not (Test-Path $clsidKey)) {
    Write-Host "TSF Input Processor unregistered successfully!" -ForegroundColor Green
    Write-Host "Registry entry cleanly removed from HKCR." -ForegroundColor Green
} else {
    Write-Warning "Unregistration finished but CLSID key is still present at $clsidKey"
}
