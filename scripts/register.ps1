<#
.SYNOPSIS
    Registers Personal IME TSF In-Proc Server DLL (x64)
.DESCRIPTION
    Safely registers the 64-bit PersonalIME.dll into Windows COM & TSF Category Manager.
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

if (-not (Test-Path $DllPath)) {
    throw "Target DLL not found at: $DllPath. Please run build.ps1 first."
}
$resolvedDll = (Resolve-Path $DllPath).Path

if ($DryRun) {
    Write-Host "[DRY-RUN] PersonalIME TSF DLL registration check:" -ForegroundColor Cyan
    Write-Host "  Target DLL: $resolvedDll" -ForegroundColor Gray
    Write-Host "  Architecture: x64" -ForegroundColor Gray
    Write-Host "  RegSvr32: $env:windir\System32\regsvr32.exe" -ForegroundColor Gray
    Write-Host "  CLSID: {7A5074A1-4F29-4FBE-A2D0-244D42B28B01}" -ForegroundColor Gray
    return
}

# Elevate privilege check
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Registering a TSF input method requires Administrator privileges. Relaunching as Admin..."
    $argString = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -DllPath `"$resolvedDll`""
    try {
        $elevated = Start-Process powershell.exe -ArgumentList $argString -Verb RunAs -WindowStyle Normal -Wait -PassThru
        exit $elevated.ExitCode
    } catch {
        Write-Warning "Auto-elevation cannot run in non-interactive environment. Please run PowerShell as Administrator once: powershell -ExecutionPolicy Bypass -File scripts\register.ps1"
        return
    }
}

Write-Host "Registering TSF DLL (x64): $resolvedDll" -ForegroundColor Cyan

# Explicit 64-bit regsvr32
$regsvr64 = Join-Path $env:windir "System32\regsvr32.exe"
if (-not (Test-Path $regsvr64)) {
    $regsvr64 = "regsvr32.exe"
}

$procUnreg = Start-Process -FilePath $regsvr64 -ArgumentList "/u /s `"$resolvedDll`"" -PassThru -Wait
$proc = Start-Process -FilePath $regsvr64 -ArgumentList "/s `"$resolvedDll`"" -PassThru -Wait
if ($proc.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code: $($proc.ExitCode)"
}

# Update CTF TIP profile description and icon in HKLM (both 64-bit and WOW6432Node)
$tipPaths = @(
    "HKLM:\SOFTWARE\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\LanguageProfile\0x00000804\{C7613511-92F2-436A-8A46-2BEF5036B959}",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\LanguageProfile\0x00000804\{C7613511-92F2-436A-8A46-2BEF5036B959}"
)
foreach ($tp in $tipPaths) {
    if (Test-Path $tp) {
        Set-ItemProperty -Path $tp -Name "Description" -Value "人人" -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $tp -Name "Display Description" -Value "人人" -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $tp -Name "IconFile" -Value "$resolvedDll" -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $tp -Name "IconIndex" -Value 0 -ErrorAction SilentlyContinue
    }
}

$clsidRootPaths = @(
    "Registry::HKEY_CLASSES_ROOT\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}",
    "HKLM:\SOFTWARE\Classes\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}"
)
foreach ($cp in $clsidRootPaths) {
    if (Test-Path $cp) {
        Set-ItemProperty -Path $cp -Name "(default)" -Value "人人" -ErrorAction SilentlyContinue
    }
}

# Post-check registry key
$clsidKey = "Registry::HKEY_CLASSES_ROOT\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}"
if (Test-Path $clsidKey) {
    Write-Host "TSF Input Processor registered successfully!" -ForegroundColor Green
    Write-Host "Registry entry verified at $clsidKey" -ForegroundColor Green
    Write-Host "You can now select '人人' in Windows Language/Keyboard settings." -ForegroundColor Yellow
} else {
    throw "Registration reported success but registry key was not found at $clsidKey"
}
