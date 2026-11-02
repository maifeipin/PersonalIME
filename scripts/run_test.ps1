$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $projectRoot "build\Release"
$serviceExe = Join-Path $binDir "ime-service.exe"
$ipcTestExe = Join-Path $binDir "test_ipc_runner.exe"
$comTestExe = Join-Path $binDir "test_com_lifecycle.exe"
$candTestExe = Join-Path $binDir "test_candidate_window.exe"
$compTestExe = Join-Path $binDir "test_composition_lifecycle.exe"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Running Automated Stage 1 Test Suite" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 1. COM DLL Lifecycle & RefCount Test
Write-Host "`n[1/5] Running COM Lifecycle & Unload Safety Test..." -ForegroundColor Yellow
& $comTestExe
if ($LASTEXITCODE -ne 0) {
    throw "COM Lifecycle test failed with exit code $LASTEXITCODE"
}

# 2. Candidate Window Class RefCount Safety Test
Write-Host "`n[2/5] Running Candidate Window RefCount Idempotency Test..." -ForegroundColor Yellow
& $candTestExe
if ($LASTEXITCODE -ne 0) {
    throw "Candidate window test failed with exit code $LASTEXITCODE"
}

# 3. Composition Lifecycle, RefCount Pairing & Fault Injection Test
Write-Host "`n[3/5] Running Composition Lifecycle, RefCount Pairing & Fault Injection Test..." -ForegroundColor Yellow
& $compTestExe
if ($LASTEXITCODE -ne 0) {
    throw "Composition lifecycle test failed with exit code $LASTEXITCODE"
}

# 4. Negative Cases (All-Zero Response, Stalled Server Timeout, Busy Pipe Budget, Harness Hang Safety)
Write-Host "`n[4/5] Running Negative / Fault Injection & Busy-Pipe Budget Tests..." -ForegroundColor Yellow
$negTestScript = Join-Path $projectRoot "tests\verify_negative_cases.ps1"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $negTestScript
if ($LASTEXITCODE -ne 0) {
    throw "Negative cases test failed with exit code $LASTEXITCODE"
}

# 5. Happy Path with live ime-service (Multi-context & Isolation)
Write-Host "`n[5/5] Running Multi-Context & Multi-Thread Isolation Happy-Path IPC Test..." -ForegroundColor Yellow
if (Get-Process ime-service -ErrorAction SilentlyContinue) {
    throw "An existing ime-service is already running. Please terminate it before testing."
}

$proc = Start-Process -FilePath $serviceExe -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru

# Wait for pipe readiness
$pipeReady = $false
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Milliseconds 80
    try {
        $pipeTest = [System.IO.Pipes.NamedPipeClientStream]::new(".", "PersonalIME_Service_Pipe", [System.IO.Pipes.PipeDirection]::InOut)
        $pipeTest.Connect(80)
        $pipeTest.Close()
        $pipeReady = $true
        break
    } catch {}
}

if (-not $pipeReady) {
    Stop-Process -Id $proc.Id -Force
    throw "Timed out waiting for ime-service pipe to become ready."
}

try {
    & $ipcTestExe
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) {
        throw "Happy-path test_ipc_runner failed with exit code $testExit"
    }
} finally {
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit(1000)
    }
}

Write-Host "`n==> ALL TESTS IN STAGE 1 PASSED WITH VERIFIED EVIDENCE!" -ForegroundColor Green
