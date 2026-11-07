<#
.SYNOPSIS
    Personal IME: Fast Local Development & Manual Testing Workflow (Independent of CI)
.DESCRIPTION
    Designed for rapid developer iteration, manual testing in Notepad, and quick feedback loops:
    1. Unlocks binaries: Closes running Notepad instances and stops background ime-service.
    2. Fast incremental build: Compiles ONLY runtime targets (PersonalIME.dll, ime-service.exe, Manager) in seconds.
    3. Safe TSF registration check: Verifies InprocServer32 registry path and registers if needed.
    4. Auto-system IME switch: Automatically activates Personal IME as default in Windows language list.
    5. Auto-service launch: Starts ime-service in the background (or visible console with -ConsoleService).
    6. Host launcher: Opens a fresh instance of Notepad ready for typing verification.
    7. Clean teardown: Allows one-click unregistration and cleanup when testing is finished.
#>
param(
    [ValidateSet("run", "test", "build", "reload", "stop", "status", "clean", "unregister", "manager")]
    [string]$Action = "run",

    [switch]$ConsoleService,    # Run ime-service in a visible console window for log debugging
    [switch]$KeepHost,          # Do not close Notepad automatically
    [switch]$BuildTests,        # Also build all 12 test binaries (default: fast runtime build only)
    [string]$BuildDir = $env:PERSONALIME_BUILD_DIR
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = "build"
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $projectRoot $BuildDir }
$releaseDir = Join-Path $resolvedBuildDir "Release"
$targetDll = Join-Path $releaseDir "PersonalIME.dll"
$serviceExe = Join-Path $releaseDir "ime-service.exe"
$managerProj = Join-Path $projectRoot "src\manager\PersonalIME.Manager\PersonalIME.Manager.csproj"
$managerBinDir = Join-Path $projectRoot "src\manager\PersonalIME.Manager\bin\Release\net10.0-windows"
$managerTargetDir = Join-Path $releaseDir "manager"
$managerExe = Join-Path $managerTargetDir "PersonalIME.Manager.exe"

$imeTip = "0804:{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}{C7613511-92F2-436A-8A46-2BEF5036B959}"

# Helper: Locate cmake.exe
function Get-CmakePath {
    $cmakeCmd = Get-Command "cmake" -ErrorAction SilentlyContinue
    if ($cmakeCmd) { return $cmakeCmd.Source }
    $vsCmakePaths = @(
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($p in $vsCmakePaths) {
        if (Test-Path $p) { return $p }
    }
    return "cmake"
}

# Helper: Check if service pipe is ready
function Test-ImePipeReady {
    $pipeName = "\\.\pipe\PersonalIME_Service_Pipe"
    return Test-Path $pipeName
}
function Test-IsAdmin {
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Helper: Set Personal IME as the active system input method
function Set-ImeActiveInSystem {
    try {
        $langList = Get-WinUserLanguageList
        $zhLang = $langList | Where-Object { $_.LanguageTag -like "zh-*" }
        if ($zhLang) {
            if ($zhLang.InputMethodTips -contains $imeTip) {
                $zhLang.InputMethodTips.Remove($imeTip) | Out-Null
            }
            $zhLang.InputMethodTips.Insert(0, $imeTip)
            Set-WinUserLanguageList $langList -Force
            Set-WinDefaultInputMethodOverride -InputTip $imeTip
            Write-Host "  Personal IME set as default input method in Windows." -ForegroundColor Green
        }
    } catch {
        Write-Warning "Could not update default input method tip: $_"
    }
}

# Helper: Remove Personal IME from Windows language list
function Reset-ImeFromSystem {
    try {
        Set-WinDefaultInputMethodOverride -InputTip "" -ErrorAction SilentlyContinue
        $langList = Get-WinUserLanguageList
        $zhLang = $langList | Where-Object { $_.LanguageTag -like "zh-*" }
        if ($zhLang -and ($zhLang.InputMethodTips -contains $imeTip)) {
            $zhLang.InputMethodTips.Remove($imeTip) | Out-Null
            Set-WinUserLanguageList $langList -Force
            Write-Host "  Removed Personal IME from Windows language list." -ForegroundColor Gray
        }
    } catch {}
}

# Helper: Find processes locking PersonalIME.dll
function Get-DllLockers {
    param([string]$DllName = "PersonalIME.dll")
    $lockers = @()
    Get-Process | ForEach-Object {
        $proc = $_
        try {
            $mod = $proc.Modules | Where-Object { $_.ModuleName -eq $DllName }
            if ($mod) {
                $lockers += [PSCustomObject]@{
                    Id = $proc.Id
                    Name = $proc.ProcessName
                    Path = $mod.FileName
                }
            }
        } catch {
            # Skip system/protected processes
        }
    }
    return $lockers
}

# Helper: Gracefully stop ime-service
function Stop-ImeService {
    $procs = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
    if ($procs) {
        Write-Host "  Stopping running ime-service (PID: $($procs.Id -join ', '))..." -ForegroundColor Yellow
        $procs | ForEach-Object {
            try { Stop-Process -Id $_.Id -Force } catch {}
        }
        Start-Sleep -Milliseconds 400
    }
}

# Helper: Close test host apps locking the DLL
function Unlock-HostProcesses {
    $lockers = Get-DllLockers "PersonalIME.dll"
    if ($lockers.Count -gt 0) {
        Write-Host "  Processes currently holding PersonalIME.dll:" -ForegroundColor Gray
        foreach ($l in $lockers) {
            Write-Host "    - $($l.Name) (PID: $($l.Id)) -> $($l.Path)" -ForegroundColor Gray
        }

        if (-not $KeepHost) {
            $testLockers = $lockers | Where-Object { $_.Name -like "*notepad*" -or $_.Name -like "*PersonalIME.Manager*" }
            if ($testLockers) {
                Write-Host "  Closing test hosts (Notepad / Manager) to release DLL file lock..." -ForegroundColor Cyan
                $testLockers | ForEach-Object {
                    try { Stop-Process -Id $_.Id -Force } catch {}
                }
                Start-Sleep -Milliseconds 600
            }
        } else {
            Write-Warning "KeepHost is enabled. If build fails due to locked DLL, please close the host application manually."
        }
    }
}

# Action: STATUS
if ($Action -eq "status") {
    Write-Host "=====================================================" -ForegroundColor Cyan
    Write-Host "  Personal IME: Dev & Manual Test Status" -ForegroundColor Cyan
    Write-Host "=====================================================" -ForegroundColor Cyan

    # 1. Registered InprocServer32
    $reg = Get-ItemProperty -Path "Registry::HKEY_CLASSES_ROOT\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\InprocServer32" -ErrorAction SilentlyContinue
    if ($reg) {
        Write-Host "[TSF Registration] Registered DLL:" -ForegroundColor Green
        Write-Host "  $($reg.'(default)')" -ForegroundColor White
        if (Test-Path $reg.'(default)') {
            $item = Get-Item $reg.'(default)'
            Write-Host "  Last Modified: $($item.LastWriteTime) (Size: $($item.Length) bytes)" -ForegroundColor Gray
        } else {
            Write-Host "  [WARNING] Registered file does NOT exist on disk!" -ForegroundColor Red
        }
    } else {
        Write-Host "[TSF Registration] NOT registered in Windows TSF." -ForegroundColor Yellow
    }

    # 2. Windows Language List & Override
    $override = (Get-WinDefaultInputMethodOverride -ErrorAction SilentlyContinue).InputTip
    if ($override -eq $imeTip) {
        Write-Host "[System Input Method] Personal IME is the DEFAULT OVERRIDE." -ForegroundColor Green
    } else {
        Write-Host "[System Input Method] Default override: $(if ($override) { $override } else { 'None (System Default)' })" -ForegroundColor Gray
    }

    # 3. Running Service
    $svc = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
    if ($svc) {
        Write-Host "`n[Service] Running (PID: $($svc.Id -join ', '))" -ForegroundColor Green
        foreach ($s in $svc) {
            Write-Host "  Path: $($s.Path)" -ForegroundColor White
            Write-Host "  Started: $($s.StartTime)" -ForegroundColor Gray
        }
    } else {
        Write-Host "`n[Service] NOT running (offline)" -ForegroundColor Yellow
    }

    # 4. DLL Loaded Modules
    $lockers = Get-DllLockers "PersonalIME.dll"
    if ($lockers.Count -gt 0) {
        Write-Host "`n[Host Processes] PersonalIME.dll loaded in $($lockers.Count) process(es):" -ForegroundColor Cyan
        foreach ($l in $lockers) {
            Write-Host "  - $($l.Name) (PID: $($l.Id)) -> $($l.Path)" -ForegroundColor White
        }
    } else {
        Write-Host "`n[Host Processes] No applications currently holding PersonalIME.dll." -ForegroundColor Gray
    }
    return
}

# Action: STOP
if ($Action -eq "stop") {
    Stop-ImeService
    Write-Host "ime-service stopped." -ForegroundColor Green
    return
}

# Action: UNREGISTER / CLEAN
if ($Action -in @("clean", "unregister")) {
    Write-Host "==> Cleaning up manual test environment..." -ForegroundColor Cyan
    Stop-ImeService
    Unlock-HostProcesses
    Reset-ImeFromSystem

    if (Test-IsAdmin) {
        regsvr32 /u /s $targetDll
        Write-Host "==> Personal IME successfully unregistered from Windows TSF." -ForegroundColor Green
    } else {
        $unregScript = Join-Path $PSScriptRoot "unregister.ps1"
        if (Test-Path $unregScript) {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $unregScript
        } else {
            Write-Warning "Unregistering requires Administrator privileges. Run in an Admin terminal or run scripts/unregister.ps1."
        }
    }
    return
}

# Action: MANAGER
if ($Action -eq "manager") {
    if (-not (Test-Path $managerExe)) {
        Write-Host "Building Manager UI..." -ForegroundColor Cyan
        dotnet build $managerProj -c Release --nologo -v q
        if (-not (Test-Path $managerTargetDir)) { New-Item -ItemType Directory -Path $managerTargetDir -Force | Out-Null }
        Copy-Item -Path "$managerBinDir\*" -Destination $managerTargetDir -Recurse -Force
    }
    Start-Process $managerExe
    return
}

# Execution Flow for "build", "reload", "run", "test":
Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host "  Personal IME: Fast Dev & Manual Test Workflow" -ForegroundColor Cyan
Write-Host "=====================================================" -ForegroundColor Cyan

# Step 1: Release Locks
Write-Host "`n[1/5] Unlocking binary files..." -ForegroundColor Cyan
Stop-ImeService
Unlock-HostProcesses

# Step 2: Incremental Build (Skipped if reload only)
if ($Action -ne "reload") {
    Write-Host "`n[2/5] Building runtime components (Release x64)..." -ForegroundColor Cyan
    $cmake = Get-CmakePath

    # Ensure build dir exists and configured
    if (-not (Test-Path (Join-Path $resolvedBuildDir "CMakeCache.txt"))) {
        Write-Host "  Configuring CMake..." -ForegroundColor Gray
        & $cmake -S $projectRoot -B $resolvedBuildDir -A x64
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    }

    # Clean up old unlocked renamed DLLs and rename currently mapped PersonalIME.dll if locked
    $releaseDir = Split-Path -Parent $targetDll
    if (Test-Path $releaseDir) {
        Get-ChildItem -Path $releaseDir -Filter "PersonalIME.dll.old*" -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path $targetDll) {
        try {
            $fs = [System.IO.File]::Open($targetDll, 'Open', 'ReadWrite', 'None')
            $fs.Close()
        } catch {
            $oldName = "$targetDll.old.$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())"
            Move-Item -Path $targetDll -Destination $oldName -Force -ErrorAction SilentlyContinue
        }
    }

    # Build targets
    $targets = if ($BuildTests) { @() } else { @("--target", "PersonalIME", "ime-service", "ime-cli") }
    & $cmake --build $resolvedBuildDir --config Release @targets
    if ($LASTEXITCODE -ne 0) { throw "Native compilation failed." }

    # Build and publish Manager UI to build\Release\manager
    Write-Host "  Building Manager UI..." -ForegroundColor Gray
    dotnet build $managerProj -c Release --nologo -v q
    if ($LASTEXITCODE -ne 0) { throw "Manager compilation failed." }

    if (-not (Test-Path $managerTargetDir)) {
        New-Item -ItemType Directory -Path $managerTargetDir -Force | Out-Null
    }
    Copy-Item -Path "$managerBinDir\*" -Destination $managerTargetDir -Recurse -Force
    $env:PERSONALIME_MANAGER_PATH = $managerExe
    Write-Host "  Manager published to: $managerTargetDir" -ForegroundColor Gray
    Write-Host "  Build completed successfully!" -ForegroundColor Green
} else {
    Write-Host "`n[2/5] Skipping compilation (Reload mode)..." -ForegroundColor Gray
}

# Step 3: Register DLL to Windows TSF & Set Active
Write-Host "`n[3/5] Verifying Windows TSF Registration & Profile..." -ForegroundColor Cyan
$targetDllResolved = (Resolve-Path $targetDll -ErrorAction SilentlyContinue).Path

# Ensure HKCU Classes CLSID always points to the active dev build DLL (no Admin required)
try {
    $hkcuClsid = "HKCU:\Software\Classes\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}"
    $hkcuInproc = "$hkcuClsid\InProcServer32"
    New-Item -Path $hkcuInproc -Force | Out-Null
    Set-ItemProperty -Path $hkcuClsid -Name "(default)" -Value "人人" -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $hkcuInproc -Name "(default)" -Value $targetDllResolved -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $hkcuInproc -Name "ThreadingModel" -Value "Apartment" -ErrorAction SilentlyContinue

    $hkcuTip = "HKCU:\Software\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\LanguageProfile\0x00000804\{C7613511-92F2-436A-8A46-2BEF5036B959}"
    New-Item -Path $hkcuTip -Force | Out-Null
    Set-ItemProperty -Path $hkcuTip -Name "Description" -Value "人人" -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $hkcuTip -Name "Display Description" -Value "人人" -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $hkcuTip -Name "IconFile" -Value $targetDllResolved -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $hkcuTip -Name "IconIndex" -Value 0 -Type DWord -ErrorAction SilentlyContinue
} catch {}

$reg = Get-ItemProperty -Path "Registry::HKEY_CLASSES_ROOT\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\InprocServer32" -ErrorAction SilentlyContinue

$tipRegKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\CTF\TIP\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\LanguageProfile\0x00000804\{C7613511-92F2-436A-8A46-2BEF5036B959}"
$tipProp = Get-ItemProperty -Path $tipRegKey -ErrorAction SilentlyContinue

$isUpToDate = $reg -and ($reg.'(default)' -eq $targetDllResolved) -and ($tipProp -and ($tipProp.Description -eq "人人"))

if ($isUpToDate) {
    Write-Host "  PersonalIME.dll is registered and up-to-date: $targetDllResolved" -ForegroundColor Green
} else {
    if (Test-IsAdmin) {
        $regScript = Join-Path $PSScriptRoot "register.ps1"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $regScript -DllPath $targetDllResolved
    } else {
        Write-Host "  Updating TSF profile registration (Administrator elevation needed)..." -ForegroundColor Yellow
        $regScript = Join-Path $PSScriptRoot "register.ps1"
        if (Test-Path $regScript) {
            try {
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $regScript -DllPath $targetDllResolved
            } catch {
                Write-Warning "Registration requires Administrator privileges. If registration failed, please run: powershell -File scripts/register.ps1"
            }
        }
    }
}

# Automatically activate in Windows Language List & Override
Set-ImeActiveInSystem

# Step 4: Start ime-service
Write-Host "`n[4/5] Starting ime-service..." -ForegroundColor Cyan
$serviceDir = Split-Path -Parent $serviceExe
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $serviceExe
$psi.WorkingDirectory = $serviceDir
$psi.UseShellExecute = $true

if ($ConsoleService) {
    Write-Host "  Starting ime-service in interactive console window..." -ForegroundColor White
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Normal
} else {
    Write-Host "  Starting ime-service in background (silent/hidden)..." -ForegroundColor White
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
}

[System.Diagnostics.Process]::Start($psi) | Out-Null

$pipeReady = $false
for ($i = 0; $i -lt 15; $i++) {
    if (Test-ImePipeReady) {
        $pipeReady = $true
        break
    }
    Start-Sleep -Milliseconds 150
}
$svcCheck = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
if ($svcCheck -and $pipeReady) {
    Write-Host "  ime-service is RUNNING (PID: $($svcCheck.Id -join ', ')) and pipe is READY!" -ForegroundColor Green
} elseif ($svcCheck) {
    Write-Host "  ime-service is RUNNING (PID: $($svcCheck.Id -join ', '))" -ForegroundColor Green
} else {
    Write-Warning "ime-service did not start. You can run '.\scripts\dev.ps1 -ConsoleService' to view startup errors."
}

# Step 5: Launch Notepad for Manual Testing
if ($Action -in @("run", "test", "reload")) {
    Write-Host "`n[5/5] Launching fresh Notepad instance..." -ForegroundColor Cyan
    Start-Process "notepad.exe"

    Write-Host "`n=====================================================" -ForegroundColor Green
    Write-Host "  Personal IME is READY for manual testing!" -ForegroundColor Green
    Write-Host "=====================================================" -ForegroundColor Green
    Write-Host "Quick Test Guide in Notepad:" -ForegroundColor White
    Write-Host "  - Windows has automatically set Personal IME as active!" -ForegroundColor Green
    Write-Host "  1. Test Wubi: Type 'aaaa' -> Shows '1. 工' with soft-blue badge" -ForegroundColor Yellow
    Write-Host "  2. Test Pinyin Fallback: Type 'quanpin' -> Cascades to '1. 全屏 2. 全凭'" -ForegroundColor Yellow
    Write-Host "  3. Test Reverse Lookup: Type 'xi'an' -> Shows candidate with slate hint" -ForegroundColor Yellow
    Write-Host "  4. Test Manager: Click the [⚙] gear button on the right of candidate window" -ForegroundColor Yellow
    Write-Host "`nUseful dev commands:" -ForegroundColor Gray
    Write-Host "  .\scripts\dev.ps1 -Action status   # Check registration, service PID, and DLL lockers" -ForegroundColor Gray
    Write-Host "  .\scripts\dev.ps1 -Action reload   # Restart service & notepad without recompilation" -ForegroundColor Gray
    Write-Host "  .\scripts\dev.ps1 -Action clean    # Stop service, cleanly unregister DLL & reset language list" -ForegroundColor Gray
    Write-Host "  .\scripts\dev.ps1 -Action manager  # Open the Manager settings window directly" -ForegroundColor Gray
}
