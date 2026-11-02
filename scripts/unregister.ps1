$ErrorActionPreference = 'Stop'
# Elevate privilege check
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Unregistering a TSF input method requires Administrator privileges. Relaunching as Admin..."
    $elevated = Start-Process powershell.exe -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"") -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    exit $elevated.ExitCode
}

$dllPath = Join-Path $PSScriptRoot "..\build\Release\PersonalIME.dll"
$resolvedDll = if (Test-Path $dllPath) { (Resolve-Path $dllPath).Path } else { $dllPath }
Write-Host "Unregistering TSF DLL: $resolvedDll" -ForegroundColor Cyan

$proc = Start-Process regsvr32.exe -ArgumentList "/u /s `"$resolvedDll`"" -PassThru -Wait
if ($proc.ExitCode -eq 0) {
    Write-Host "TSF Input Processor unregistered successfully!" -ForegroundColor Green
} else {
    throw "regsvr32 /u failed with exit code: $($proc.ExitCode)"
}

