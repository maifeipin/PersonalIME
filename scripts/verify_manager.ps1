# scripts/verify_manager.ps1
# Stage 4C Verification Script for PersonalIME WPF Desktop Manager & Admin IPC Client

$ErrorActionPreference = "Stop"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Stage 4C Verification: PersonalIME Desktop Manager (WPF)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$OrigLocation = Get-Location
$RepoRoot = (Get-Item $PSScriptRoot).Parent.FullName

try {
    Set-Location $RepoRoot

    # 1. Build C++ Backend binaries
    Write-Host "`n[1/3] Building C++ Backend (ime-service, test_admin_ipc)..." -ForegroundColor Yellow
    $cmake = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) {
        $cmake = "cmake"
    }

    & $cmake --build build --config Release --target ime-service test_admin_ipc
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to build C++ backend targets"
        exit 1
    }

    # 2. Build WPF Manager & Tests
    Write-Host "`n[2/3] Building PersonalIME.Manager & Tests (WPF .NET 10)..." -ForegroundColor Yellow
    dotnet build src\manager\PersonalIME.Manager.Tests\PersonalIME.Manager.Tests.csproj -c Release
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to build PersonalIME.Manager.Tests"
        exit 1
    }

    # 3. Run Test Suite with TRX logger for accurate result parsing
    Write-Host "`n[3/3] Running MSTest Suite in PersonalIME.Manager.Tests..." -ForegroundColor Yellow
    $ResultsDir = Join-Path $RepoRoot "out\test-results"
    if (Test-Path $ResultsDir) { Remove-Item -Recurse -Force $ResultsDir }
    New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

    $TrxPath = Join-Path $ResultsDir "manager-tests.trx"
    dotnet test src\manager\PersonalIME.Manager.Tests\PersonalIME.Manager.Tests.csproj -c Release --no-build --logger "trx;LogFileName=manager-tests.trx" --results-directory $ResultsDir
    $TestExitCode = $LASTEXITCODE

    if (-not (Test-Path $TrxPath)) {
        Write-Error "Test results TRX file not generated: $TrxPath"
        exit 1
    }

    [xml]$trx = Get-Content $TrxPath
    $counters = $trx.TestRun.ResultSummary.Counters
    $total = [int]$counters.total
    $passed = [int]$counters.passed
    $failed = [int]$counters.failed
    $skipped = [int]$counters.notExecuted + [int]$counters.ignored

    Write-Host "`nTest Execution Summary: Total: $total, Passed: $passed, Failed: $failed, Skipped: $skipped" -ForegroundColor White

    if ($TestExitCode -ne 0 -or $failed -gt 0 -or $passed -eq 0 -or $skipped -gt 0) {
        Write-Error "Verification failed: $failed failed, $skipped skipped, $passed passed out of $total total tests."
        exit 1
    }

    Write-Host "`n==========================================================" -ForegroundColor Green
    Write-Host "  ALL Stage 4C Desktop Manager Tests PASSED! ($passed/$total passed, 0 skipped)" -ForegroundColor Green
    Write-Host "==========================================================" -ForegroundColor Green
}
finally {
    Set-Location $OrigLocation
}

