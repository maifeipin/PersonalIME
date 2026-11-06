<#
.SYNOPSIS
    Personal IME Unified CI & Pre-Release Regression Quality Gate
.DESCRIPTION
    Runs:
    1. Standalone native C++ unit and engine tests (10 binaries)
    2. Negative IPC protocol & fault-injection tests (verify_negative_cases.ps1)
    3. Stage 3 & 4C-2 End-to-End integration tests (test_stage3_integration.exe)
    4. Service-backed native IPC tests (test_ipc_runner.exe) with isolated background service
    5. WPF Desktop Manager MSTest suite with TRX parsing (0 failures, 0 skips, >= 40 passed)
    Strictly halts and exits with non-zero code on any single failure.
#>

param(
    [switch]$SkipWpf
)

$ErrorActionPreference = "Stop"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptRoot
$BinDir = Join-Path $ProjectRoot "build\Release"
$GateTestDir = Join-Path $ProjectRoot "out\ci_gate_test"
$TrxPath = Join-Path $ProjectRoot "out\test-results\ci-manager-tests.trx"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Personal IME: Unified CI / Pre-Release Quality Gate" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Ensure test directories exist
if (-not (Test-Path (Join-Path $ProjectRoot "out\test-results"))) {
    New-Item -ItemType Directory -Path (Join-Path $ProjectRoot "out\test-results") -Force | Out-Null
}
if (-not (Test-Path $GateTestDir)) {
    New-Item -ItemType Directory -Path $GateTestDir -Force | Out-Null
}

function Run-NativeTest {
    param([string]$ExeName)
    $exePath = Join-Path $BinDir $ExeName
    if (-not (Test-Path $exePath)) {
        throw "Test binary missing: $exePath. Ensure C++ project is built."
    }
    Write-Host "`n==> [RUN] $ExeName" -ForegroundColor Cyan
    & $exePath
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "CRITICAL FAILURE: $ExeName failed with exit code $code"
    }
    Write-Host "==> [PASS] $ExeName" -ForegroundColor Green
}

# Collision check: ensure no residual service is running before test suite starts
$preExisting = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
if ($preExisting) {
    throw "Cannot run quality gate: An existing ime-service (PIDs: $(($preExisting | ForEach-Object {$_.Id}) -join ',')) is currently running."
}

# --- Section 1: Standalone Native Tests ---
Write-Host "`n>>> [SECTION 1/5] Standalone Native Unit & Engine Tests <<<" -ForegroundColor Yellow

$standaloneTests = @(
    "test_ime_core.exe",
    "test_candidate_window.exe",
    "test_com_lifecycle.exe",
    "test_receipt_journal.exe",
    "test_receipt_handover.exe",
    "test_receipt_durability.exe",
    "test_input_policy.exe",
    "test_admin_ipc.exe",
    "test_composition_lifecycle.exe",
    "test_tip_candidate_snapshot.exe"
)

foreach ($t in $standaloneTests) {
    Run-NativeTest -ExeName $t
}

# --- Section 2: Negative IPC Protocol & Fault-Injection Tests ---
Write-Host "`n>>> [SECTION 2/5] Negative IPC Protocol & Fault-Injection Tests <<<" -ForegroundColor Yellow
$negTestScript = Join-Path $ProjectRoot "tests\verify_negative_cases.ps1"
if (-not (Test-Path $negTestScript)) {
    throw "Negative cases test script missing: $negTestScript"
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $negTestScript
if ($LASTEXITCODE -ne 0) {
    throw "CRITICAL FAILURE: Negative IPC fault injection tests failed with exit code $LASTEXITCODE"
}
Write-Host "==> [PASS] Negative IPC Protocol & Fault-Injection Tests" -ForegroundColor Green

# Ensure no residual pipe or process
$residualPostNeg = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
if ($residualPostNeg) {
    throw "Residual ime-service found after negative tests: $(($residualPostNeg | ForEach-Object {$_.Id}) -join ',')"
}

# --- Section 3: Stage 3 & 4C-2 End-to-End Native Integration Tests ---
Write-Host "`n>>> [SECTION 3/5] Stage 3 & 4C-2 End-to-End Native Integration Tests <<<" -ForegroundColor Yellow
Push-Location -LiteralPath $ProjectRoot
try {
    # Remove any stale integration db
    Get-ChildItem -Path $ProjectRoot -Include "stage3_integration_test.db*" -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
    
    Run-NativeTest -ExeName "test_stage3_integration.exe"
} finally {
    Get-ChildItem -Path $ProjectRoot -Include "stage3_integration_test.db*" -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
    Pop-Location
}

# Ensure service spawned by integration test terminated cleanly
$residualPostInteg = Get-Process -Name "ime-service" -ErrorAction SilentlyContinue
if ($residualPostInteg) {
    Write-Host "Cleaning up residual ime-service after integration test..." -ForegroundColor Yellow
    $residualPostInteg | Stop-Process -Force -ErrorAction SilentlyContinue
}

# --- Section 4: Service-Backed Native IPC Tests ---
Write-Host "`n>>> [SECTION 4/5] Service-Backed IPC Integration Tests <<<" -ForegroundColor Yellow

# Clean old gate databases
Get-ChildItem -Path $GateTestDir -Include "*.db*", "*.pid", "*.json" -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue

$serviceExe = Join-Path $BinDir "ime-service.exe"
$gateDb = Join-Path $GateTestDir "ci_user_dict.db"
$gateDict = Join-Path $ProjectRoot "data\dictionaries"

Write-Host "Starting isolated background service for IPC test..." -ForegroundColor Cyan
$svcProc = Start-Process -FilePath $serviceExe -ArgumentList @("--db", "$gateDb", "--dict", "$gateDict") -PassThru

if (-not $svcProc -or $svcProc.HasExited) {
    throw "Failed to start ime-service for CI gate."
}

$pipeReady = $false
try {
    Write-Host "Waiting for IPC service pipe to become ready..." -NoNewline
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path "\\.\pipe\PersonalIME_Service_Pipe") {
            $pipeReady = $true
            break
        }
    }
    if (-not $pipeReady) {
        throw "Timed out waiting for PersonalIME_Service_Pipe to become available."
    }
    Write-Host " READY!" -ForegroundColor Green

    # Execute test_ipc_runner
    Run-NativeTest -ExeName "test_ipc_runner.exe"

} finally {
    # Strictly terminate ONLY our isolated service
    if ($svcProc -and -not $svcProc.HasExited) {
        Write-Host "Stopping isolated CI test service (PID $($svcProc.Id))..." -ForegroundColor Cyan
        Stop-Process -Id $svcProc.Id -Force -ErrorAction SilentlyContinue
        $svcProc.WaitForExit(1000)
    }
}

# --- Section 5: WPF Desktop Manager MSTest Suite ---
if (-not $SkipWpf) {
    Write-Host "`n>>> [SECTION 5/5] WPF Manager MSTest Suite & TRX Validation <<<" -ForegroundColor Yellow

    $managerTestProj = Join-Path $ProjectRoot "src\manager\PersonalIME.Manager.Tests\PersonalIME.Manager.Tests.csproj"
    if (Test-Path $TrxPath) { Remove-Item -Path $TrxPath -Force }

    Write-Host "Running dotnet test with TRX logging..." -ForegroundColor Cyan
    & dotnet test $managerTestProj -c Release --logger "trx;LogFileName=ci-manager-tests.trx" --results-directory (Join-Path $ProjectRoot "out\test-results")
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet test exited with non-zero code $LASTEXITCODE"
    }

    if (-not (Test-Path $TrxPath)) {
        throw "TRX test result file not found at: $TrxPath"
    }

    # Parse TRX XML to verify 0 failures, 0 skips, and 0 inconclusive
    [xml]$trx = Get-Content -LiteralPath $TrxPath
    $counters = $trx.TestRun.ResultSummary.Counters

    $total = [int]$counters.total
    $passed = [int]$counters.passed
    $failed = [int]$counters.failed
    $skipped = [int]$counters.notExecuted + [int]$counters.pending
    $inconclusive = [int]$counters.inconclusive

    Write-Host "`nWPF Test Summary from TRX:" -ForegroundColor Cyan
    Write-Host "  Total: $total | Passed: $passed | Failed: $failed | Skipped: $skipped | Inconclusive: $inconclusive" -ForegroundColor White

    if ($failed -gt 0) {
        throw "TRX Validation Failed: $failed tests failed in PersonalIME.Manager.Tests!"
    }
    if ($skipped -gt 0) {
        throw "TRX Validation Failed: $skipped tests were skipped in PersonalIME.Manager.Tests!"
    }
    if ($inconclusive -gt 0) {
        throw "TRX Validation Failed: $inconclusive tests were inconclusive in PersonalIME.Manager.Tests!"
    }
    if ($passed -lt 40) {
        throw "TRX Validation Failed: Expected at least 40 tests, but only $passed ran."
    }
    Write-Host "TRX Quality Gate PASSED: 100% tests passed with 0 failures and 0 skips." -ForegroundColor Green
}

Write-Host "`n==========================================================" -ForegroundColor Green
Write-Host "  ALL AUTOMATED CI GATES PASSED (100% SUCCESS)           " -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
Write-Host "NOTE: This gate validates automated logic, IPC framing," -ForegroundColor Yellow
Write-Host "TSF state machines, and WPF ViewModels. Real host text " -ForegroundColor Yellow
Write-Host "acceptance (Notepad, Chromium, VS Code, Terminal) must" -ForegroundColor Yellow
Write-Host "be verified per docs/stage5-host-acceptance.md." -ForegroundColor Cyan
