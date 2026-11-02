$ErrorActionPreference = 'Stop'
# Elevate privilege check
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Registering a TSF input method requires Administrator privileges. Relaunching as Admin..."
    $elevated = Start-Process powershell.exe -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"") -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    exit $elevated.ExitCode
}

$dllPath = Join-Path $PSScriptRoot "..\build\Release\PersonalIME.dll"
if (-not (Test-Path $dllPath)) {
    Write-Error "PersonalIME.dll not found at $dllPath. Please run build.ps1 first."
    exit 1
}

$resolvedDll = (Resolve-Path $dllPath).Path
Write-Host "Registering TSF DLL: $resolvedDll" -ForegroundColor Cyan

# Run regsvr32
$proc = Start-Process regsvr32.exe -ArgumentList "/s `"$resolvedDll`"" -PassThru -Wait
if ($proc.ExitCode -eq 0) {
    Write-Host "TSF Input Processor registered successfully!" -ForegroundColor Green
    Write-Host "You can now see 'Personal IME (私人输入法)' in Windows Language/Keyboard settings." -ForegroundColor Yellow
} else {
    throw "regsvr32 failed with exit code: $($proc.ExitCode)"
}

