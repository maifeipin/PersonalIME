<#
.SYNOPSIS
    Personal IME Stage 5 Host Acceptance Environment & Diagnostics Tool
.DESCRIPTION
    Provides pre-registration checks, test service management (with strict PID isolation),
    status diagnostics, registration audit, and cleanup tools for host acceptance.
    Strictly avoids interfering with preexisting user processes.
#>

param(
    [ValidateSet("CheckPrereqs", "StartService", "StopService", "Status", "AuditRegistration", "Cleanup")]
    [string]$Action = "Status",
    [string]$DbPath = "",
    [string]$DictDir = "",
    [string]$DllPath = ""
)

$ErrorActionPreference = "Stop"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptRoot
$TestDir = Join-Path $ProjectRoot "out\stage5_host_test"
$PidFile = Join-Path $TestDir "test_service.pid"
$MetadataFile = Join-Path $TestDir "test_service.json"
$PreexistingPidFile = Join-Path $TestDir "preexisting_services.pid"

$ClsidString = "{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}"
$ProfileGuidString = "{C7613511-92F2-436A-8A46-2BEF5036B959}"

function Ensure-Directories {
    if (-not (Test-Path $TestDir)) {
        New-Item -ItemType Directory -Path $TestDir -Force | Out-Null
    }
}

function Test-Prerequisites {
    Write-Host "==========================================================" -ForegroundColor Cyan
    Write-Host "  Personal IME Host Acceptance: Prerequisites & Audit" -ForegroundColor Cyan
    Write-Host "==========================================================" -ForegroundColor Cyan

    $allPassed = $true

    # 1. Architecture Check
    Write-Host -NoNewline "[1/6] OS Architecture: "
    if ([Environment]::Is64BitOperatingSystem) {
        Write-Host "PASS (64-bit Windows detected. Support scope: x64 ONLY)" -ForegroundColor Green
    } else {
        Write-Host "FAIL (32-bit Windows detected. Personal IME currently requires x64)" -ForegroundColor Red
        $allPassed = $false
    }

    # 2. Administrative Privilege
    Write-Host -NoNewline "[2/6] Process Privilege: "
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        Write-Host "Administrator (Can register/unregister TSF TIP)" -ForegroundColor Green
    } else {
        Write-Host "Standard User (Registration requires Admin elevation; Service runs standard)" -ForegroundColor Yellow
    }

    # 3. Binaries Check
    Write-Host "[3/6] Binary Artifacts Integrity:"
    $binaries = @(
        @{ Name = "TSF In-Proc DLL"; Path = Join-Path $ProjectRoot "build\Release\PersonalIME.dll" },
        @{ Name = "Background Service"; Path = Join-Path $ProjectRoot "build\Release\ime-service.exe" },
        @{ Name = "CLI Tool"; Path = Join-Path $ProjectRoot "build\Release\ime-cli.exe" },
        @{ Name = "Pinyin Dictionary"; Path = Join-Path $ProjectRoot "data\dictionaries\pinyin.dict" },
        @{ Name = "Wubi86 Dictionary"; Path = Join-Path $ProjectRoot "data\dictionaries\wubi86.dict" },
        @{ Name = "WPF Manager (Published)"; Path = Join-Path $ProjectRoot "out\publish\manager\PersonalIME.Manager.exe" }
    )

    foreach ($b in $binaries) {
        if (Test-Path $b.Path) {
            $hash = (Get-FileHash -Algorithm SHA256 $b.Path).Hash.Substring(0, 16) + "..."
            $size = (Get-Item $b.Path).Length
            Write-Host "      + $($b.Name): Found ($size bytes, SHA256: $hash)" -ForegroundColor Green
        } else {
            Write-Host "      - $($b.Name): MISSING at $($b.Path)" -ForegroundColor Red
            $allPassed = $false
        }
    }

    # 4. TSF COM & Category Registration Audit
    Write-Host -NoNewline "[4/6] TSF Registry Registration: "
    $clsidPath = "Registry::HKEY_CLASSES_ROOT\CLSID\$ClsidString"
    if (Test-Path $clsidPath) {
        $inprocPath = "$clsidPath\InProcServer32"
        $dll = (Get-ItemProperty -Path $inprocPath -ErrorAction SilentlyContinue).'(default)'
        $threading = (Get-ItemProperty -Path $inprocPath -ErrorAction SilentlyContinue).'ThreadingModel'
        Write-Host "REGISTERED" -ForegroundColor Green
        Write-Host "      InProcServer32: $dll" -ForegroundColor Gray
        Write-Host "      ThreadingModel: $threading" -ForegroundColor Gray
    } else {
        Write-Host "NOT REGISTERED (Clean state)" -ForegroundColor Yellow
    }

    # 5. Preexisting ime-service Processes Detection
    Write-Host -NoNewline "[5/6] Preexisting ime-service processes: "
    Ensure-Directories
    $existing = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
    if ($existing) {
        $pids = ($existing | ForEach-Object { $_.Id }) -join ", "
        Write-Host "DETECTED (PIDs: $pids)" -ForegroundColor Yellow
        Write-Host "      [SAFETY GUARD] These preexisting processes are RECORDED and will NEVER be touched or killed by test scripts." -ForegroundColor Yellow
        $existing | ForEach-Object { $_.Id } | Out-File -FilePath $PreexistingPidFile -Encoding utf8
    } else {
        Write-Host "None (Clean state)" -ForegroundColor Green
        if (Test-Path $PreexistingPidFile) { Remove-Item -Path $PreexistingPidFile -Force }
    }

    # 6. Pipe Availability
    Write-Host -NoNewline "[6/6] IPC Pipes Status: "
    $servicePipe = Test-Path "\\.\pipe\PersonalIME_Service_Pipe"
    $adminPipe = Test-Path "\\.\pipe\PersonalIME_Admin_Pipe"
    if ($servicePipe -or $adminPipe) {
        Write-Host "ACTIVE (Service: $servicePipe, Admin: $adminPipe)" -ForegroundColor Yellow
    } else {
        Write-Host "IDLE (Pipes not yet open)" -ForegroundColor Green
    }

    return $allPassed
}

function Start-TestService {
    Ensure-Directories
    $serviceExe = Join-Path $ProjectRoot "build\Release\ime-service.exe"
    if (-not (Test-Path $serviceExe)) {
        throw "ime-service.exe not found at $serviceExe. Build project first."
    }

    # 1. Collision Guard: Check if any ime-service or pipes are already active
    $runningServices = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
    $servicePipeActive = Test-Path "\\.\pipe\PersonalIME_Service_Pipe"
    $adminPipeActive = Test-Path "\\.\pipe\PersonalIME_Admin_Pipe"

    if ($runningServices -or $servicePipeActive -or $adminPipeActive) {
        $pids = if ($runningServices) { ($runningServices | ForEach-Object { $_.Id }) -join ", " } else { "None" }
        throw "Cannot start test ime-service: Existing service process or pipe endpoint is already active (Running PIDs: $pids, ServicePipe: $servicePipeActive, AdminPipe: $adminPipeActive). To ensure strict endpoint isolation and prevent data collision, terminate existing service instances before launching an isolated test session."
    }

    # Prepare isolated paths
    $targetDb = if ($DbPath) { $DbPath } else { Join-Path $TestDir "test_user_dict.db" }
    $targetDict = if ($DictDir) { $DictDir } else { Join-Path $ProjectRoot "data\dictionaries" }

    Write-Host "Starting test ime-service with isolated parameters..." -ForegroundColor Cyan
    Write-Host "  DB Path  : $targetDb" -ForegroundColor Gray
    Write-Host "  Dict Dir : $targetDict" -ForegroundColor Gray

    $argsList = @("--db", "$targetDb", "--dict", "$targetDict")
    $proc = Start-Process -FilePath $serviceExe -ArgumentList $argsList -PassThru

    if (-not $proc -or $proc.HasExited) {
        throw "Failed to start ime-service.exe (ExitCode: $($proc.ExitCode))"
    }

    # 2. Record full metadata (PID, process name, binary path, start time) for strict identity verification
    $meta = @{
        pid = $proc.Id
        process_name = $proc.ProcessName
        executable_path = (Resolve-Path $serviceExe).Path
        start_time = $proc.StartTime.ToString("o")
        db_path = $targetDb
        dict_dir = $targetDict
        service_pipe = "\\.\pipe\PersonalIME_Service_Pipe"
        admin_pipe = "\\.\pipe\PersonalIME_Admin_Pipe"
    }
    $meta | ConvertTo-Json | Set-Content -Path $MetadataFile -Encoding utf8
    Set-Content -Path $PidFile -Value $proc.Id -Encoding utf8
    Write-Host "Test ime-service launched with PID: $($proc.Id) (ProcessName: $($proc.ProcessName))" -ForegroundColor Green

    # 3. Wait for pipe readiness and perform semantic handshake with database verification
    Write-Host "Waiting for IPC pipes and verifying service handshake..." -NoNewline
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path "\\.\pipe\PersonalIME_Service_Pipe") -and (Test-Path "\\.\pipe\PersonalIME_Admin_Pipe")) {
            # Perform semantic handshake: query GetStatus via Admin Pipe and verify db_path
            try {
                $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(".", "PersonalIME_Admin_Pipe", [System.IO.Pipes.PipeDirection]::InOut)
                $pipe.Connect(150)
                $reqPayload = '{"action":"GetStatus"}'
                $reqBytes = [System.Text.Encoding]::UTF8.GetBytes($reqPayload)
                $frame = [byte[]]::new(16 + $reqBytes.Length)
                [System.Text.Encoding]::ASCII.GetBytes("PIMA").CopyTo($frame, 0)
                [BitConverter]::GetBytes([uint32]0x0100).CopyTo($frame, 4)
                [BitConverter]::GetBytes([uint32]$reqBytes.Length).CopyTo($frame, 8)
                [BitConverter]::GetBytes([uint32]1).CopyTo($frame, 12)
                $reqBytes.CopyTo($frame, 16)
                $pipe.Write($frame, 0, $frame.Length)
                $pipe.Flush()

                $hdr = [byte[]]::new(16)
                $hRead = 0
                while ($hRead -lt 16) {
                    $r = $pipe.Read($hdr, $hRead, 16 - $hRead)
                    if ($r -le 0) { break }
                    $hRead += $r
                }
                if ($hRead -eq 16) {
                    $pLen = [BitConverter]::ToUInt32($hdr, 8)
                    $pBytes = [byte[]]::new($pLen)
                    $pRead = 0
                    while ($pRead -lt $pLen) {
                        $r = $pipe.Read($pBytes, $pRead, $pLen - $pRead)
                        if ($r -le 0) { break }
                        $pRead += $r
                    }
                    $respObj = [System.Text.Encoding]::UTF8.GetString($pBytes) | ConvertFrom-Json
                    if ($respObj.success -eq $true -and $respObj.data.db_path) {
                        $actualDb = [System.IO.Path]::GetFullPath($respObj.data.db_path)
                        $expectedDb = [System.IO.Path]::GetFullPath($targetDb)
                        if ($actualDb -eq $expectedDb) {
                            $ready = $true
                            $pipe.Dispose()
                            break
                        }
                    }
                }
                $pipe.Dispose()
            } catch {
                # Retry until deadline
            }
        }
    }

    if ($ready) {
        Write-Host " READY! (Verified connection & DB: $targetDb)" -ForegroundColor Green
    } else {
        Write-Host " TIMEOUT / HANDSHAKE FAILED (Could not verify connection or database binding within 5s)" -ForegroundColor Red
        Stop-TestService
        throw "ime-service started (PID $($proc.Id)) but semantic readiness handshake failed."
    }

    return $proc.Id
}

function Stop-TestService {
    if (-not (Test-Path $PidFile) -and -not (Test-Path $MetadataFile)) {
        Write-Host "No managed test service PID/Metadata file found." -ForegroundColor Yellow
        return
    }

    $targetPid = if (Test-Path $PidFile) { Get-Content $PidFile -ErrorAction SilentlyContinue } else { $null }
    if (-not $targetPid -and (Test-Path $MetadataFile)) {
        try {
            $meta = Get-Content $MetadataFile -Raw | ConvertFrom-Json
            $targetPid = $meta.pid
        } catch {}
    }

    if (-not $targetPid) {
        Remove-Item -Path $PidFile, $MetadataFile -Force -ErrorAction SilentlyContinue
        return
    }

    # 1. Check if process exists
    $proc = Get-Process -Id $targetPid -ErrorAction SilentlyContinue
    if (-not $proc) {
        Write-Host "Test service (PID $targetPid) is not running or already terminated." -ForegroundColor Gray
        Remove-Item -Path $PidFile, $MetadataFile -Force -ErrorAction SilentlyContinue
        return
    }

    # 2. Strict Process Name Verification (Guards against killing arbitrary child processes)
    if ($proc.ProcessName -ne "ime-service") {
        Write-Warning "[SECURITY VIOLATION] Target PID $targetPid is '$($proc.ProcessName)', NOT 'ime-service'! Refusing to terminate."
        Remove-Item -Path $PidFile, $MetadataFile -Force -ErrorAction SilentlyContinue
        return
    }

    # 3. Executable Path Verification
    try {
        $procPath = $proc.MainModule.FileName
        if ($procPath -and -not ($procPath -like "*ime-service.exe*")) {
            Write-Warning "[SECURITY VIOLATION] Target PID $targetPid binary '$procPath' does not match ime-service! Refusing to terminate."
            Remove-Item -Path $PidFile, $MetadataFile -Force -ErrorAction SilentlyContinue
            return
        }
    } catch {
        # Proceed if module path query is restricted by OS
    }

    # 4. Start Time Verification (Guards against Windows PID recycling)
    if (Test-Path $MetadataFile) {
        try {
            $meta = Get-Content $MetadataFile -Raw | ConvertFrom-Json
            if ($meta.start_time) {
                $recordedTime = [DateTime]::Parse($meta.start_time)
                $timeDiff = [Math]::Abs(($proc.StartTime - $recordedTime).TotalSeconds)
                if ($timeDiff -gt 10.0) {
                    Write-Warning "[SECURITY VIOLATION] Target PID $targetPid was recycled by Windows (Start time mismatch: current $($proc.StartTime) vs recorded $($recordedTime)). Refusing to terminate."
                    Remove-Item -Path $PidFile, $MetadataFile -Force -ErrorAction SilentlyContinue
                    return
                }
            }
        } catch {
            Write-Warning "Could not parse metadata file; verified via ProcessName."
        }
    }

    # All checks passed: Safely stop the test service
    Write-Host "Stopping verified test ime-service (PID $targetPid, Name: $($proc.ProcessName))..." -ForegroundColor Cyan
    Stop-Process -Id $targetPid -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    Write-Host "Test service (PID $targetPid) terminated." -ForegroundColor Green

    Remove-Item -Path $PidFile, $MetadataFile -Force -ErrorAction SilentlyContinue
}

function Get-Status {
    Write-Host "==========================================================" -ForegroundColor Cyan
    Write-Host "  Personal IME Host Test Status & Diagnostics" -ForegroundColor Cyan
    Write-Host "==========================================================" -ForegroundColor Cyan

    $managedPid = if (Test-Path $PidFile) { Get-Content $PidFile } else { $null }
    $allServices = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue

    if ($allServices) {
        Write-Host "Running ime-service processes:"
        foreach ($p in $allServices) {
            $isManaged = ($managedPid -and ($p.Id -eq [int]$managedPid))
            $tag = if ($isManaged) { "[MANAGED TEST SERVICE]" } else { "[EXTERNAL / PREEXISTING]" }
            $mem = [math]::Round($p.WorkingSet64 / 1MB, 2)
            Write-Host "  - PID $($p.Id): $tag WorkingSet: ${mem} MB, Threads: $($p.Threads.Count)" -ForegroundColor $(if ($isManaged) { "Green" } else { "Yellow" })
        }
    } else {
        Write-Host "No ime-service process running." -ForegroundColor Gray
    }

    Write-Host "`nIPC Pipe Status:"
    Write-Host "  - Service Pipe (\\.\pipe\PersonalIME_Service_Pipe): $(if (Test-Path '\\.\pipe\PersonalIME_Service_Pipe') { 'ACTIVE' } else { 'CLOSED' })"
    Write-Host "  - Admin Pipe   (\\.\pipe\PersonalIME_Admin_Pipe):   $(if (Test-Path '\\.\pipe\PersonalIME_Admin_Pipe') { 'ACTIVE' } else { 'CLOSED' })"

    $clsidPath = "Registry::HKEY_CLASSES_ROOT\CLSID\$ClsidString"
    Write-Host "`nTSF Registration Status:"
    Write-Host "  - HKCR\CLSID\${ClsidString}: $(if (Test-Path $clsidPath) { 'REGISTERED' } else { 'NOT REGISTERED' })"
}

function Audit-Registration {
    Write-Host "==========================================================" -ForegroundColor Cyan
    Write-Host "  TSF Input Processor Registration Audit (Read-Only)" -ForegroundColor Cyan
    Write-Host "==========================================================" -ForegroundColor Cyan

    $clsidPath = "Registry::HKEY_CLASSES_ROOT\CLSID\$ClsidString"
    if (-not (Test-Path $clsidPath)) {
        Write-Host "CLSID $ClsidString is NOT registered in HKEY_CLASSES_ROOT." -ForegroundColor Yellow
        return
    }

    $clsidKey = Get-ItemProperty -Path $clsidPath
    Write-Host "CLSID Display Name: $($clsidKey.'(default)')" -ForegroundColor Green

    $inprocPath = Join-Path $clsidPath "InProcServer32"
    if (Test-Path $inprocPath) {
        $inprocKey = Get-ItemProperty -Path $inprocPath
        Write-Host "InProcServer32 DLL: $($inprocKey.'(default)')" -ForegroundColor Green
        Write-Host "ThreadingModel    : $($inprocKey.'ThreadingModel')" -ForegroundColor Green
        if (Test-Path $inprocKey.'(default)') {
            Write-Host "DLL File Exists   : YES" -ForegroundColor Green
        } else {
            Write-Host "DLL File Exists   : NO (Broken Path!)" -ForegroundColor Red
        }
    }

    # Query registry for TSF Categories
    $catPath = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\CTF\TIP\$ClsidString\Category\Category\{FCA65D94-52C1-414E-AC75-F8194452D3B4}"
    if (Test-Path $catPath) {
        Write-Host "TIP Category Registered: YES (GUID_TFCAT_TIP_KEYBOARD)" -ForegroundColor Green
    }
}

function Clear-TestEnvironment {
    Write-Host "Cleaning up host acceptance test environment..." -ForegroundColor Cyan
    Stop-TestService
    if (Test-Path $TestDir) {
        # Retain logs, remove test databases and WAL
        Get-ChildItem -Path $TestDir -Include "*.db", "*.db-wal", "*.db-shm", "*.pid" -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
        Write-Host "Cleaned test databases and pid files in $TestDir." -ForegroundColor Green
    }
}

switch ($Action) {
    "CheckPrereqs"      { Test-Prerequisites }
    "StartService"      { Start-TestService }
    "StopService"       { Stop-TestService }
    "Status"            { Get-Status }
    "AuditRegistration" { Audit-Registration }
    "Cleanup"           { Clear-TestEnvironment }
}
