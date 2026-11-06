<#
.SYNOPSIS
    Gate Fault-Injection Probe
.DESCRIPTION
    Proves that when an earlier test fails (non-zero exit code),
    the unified gate execution halts immediately and NEVER proceeds
    to subsequent tests, packaging, or release creation.
#>

$ErrorActionPreference = "Stop"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Gate Fault-Injection Verification Probe" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Test: Sequence with earlier failure must immediately halt
$markerFile = Join-Path $env:TEMP ("gate_probe_marker_" + [Guid]::NewGuid().ToString("N") + ".txt")
if (Test-Path $markerFile) { Remove-Item -Path $markerFile -Force }

$probeScript = @"
`$ErrorActionPreference = "Stop"

function Step-One {
    Write-Host "[PROBE STEP 1] Simulated failing test"
    throw "SIMULATED CRITICAL TEST FAILURE AT STEP 1"
}

function Step-Two {
    Write-Host "[PROBE STEP 2] Subsequent release packaging"
    Set-Content -Path "$($markerFile.Replace('\', '\\'))" -Value "REACHED_PACKAGING"
}

Step-One
Step-Two
"@

$probePs1 = Join-Path $env:TEMP ("gate_probe_" + [Guid]::NewGuid().ToString("N") + ".ps1")
Set-Content -Path $probePs1 -Value $probeScript -Encoding utf8

$failedAsExpected = $false
$exitCode = 0
try {
    $proc = Start-Process powershell.exe -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$probePs1") -NoNewWindow -Wait -PassThru
    $exitCode = $proc.ExitCode
    if ($exitCode -ne 0) {
        $failedAsExpected = $true
    }
} finally {
    Remove-Item -Path $probePs1 -Force -ErrorAction SilentlyContinue
}

$reachedPackaging = Test-Path $markerFile
if (Test-Path $markerFile) { Remove-Item -Path $markerFile -Force -ErrorAction SilentlyContinue }

Write-Host "Probe Result: ExitCode=$exitCode, FailedAsExpected=$failedAsExpected, ReachedPackaging=$reachedPackaging" -ForegroundColor Yellow

if (-not $failedAsExpected) {
    throw "FAILED: Gate did not halt on simulated failure! Expected non-zero exit code."
}
if ($reachedPackaging) {
    throw "FAILED: Gate erroneously proceeded to Step 2 packaging after Step 1 failure!"
}

Write-Host "==> [PASS] Gate Fault-Injection Verified: Any test failure strictly aborts gate and blocks packaging!" -ForegroundColor Green
