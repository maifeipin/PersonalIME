$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $projectRoot ('out\stage3-verification-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$previousDb = $env:PERSONAL_IME_DB_PATH
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
if (Get-Process -Name ime-service -ErrorAction SilentlyContinue) {
    throw 'An existing ime-service must be closed before isolated verification.'
}
Push-Location -LiteralPath $projectRoot
try {
    $env:PERSONAL_IME_DB_PATH = Join-Path $runDir 'stage1.db'
    $env:TEMP = $runDir
    $env:TMP = $runDir
    & (Join-Path $PSScriptRoot 'build.ps1') -RunTests
    foreach ($name in @('test_receipt_journal', 'test_receipt_handover', 'test_receipt_durability', 'test_tip_candidate_snapshot', 'test_input_policy', 'test_admin_ipc')) {
        & (Join-Path $projectRoot "build\Release\$name.exe")
        if ($LASTEXITCODE -ne 0) { throw "$name failed with exit code $LASTEXITCODE" }
    }
    & (Join-Path $PSScriptRoot 'run_test.ps1')
    Write-Host "Stage 3 verification passed. Isolated data: $runDir"
} finally {
    $env:PERSONAL_IME_DB_PATH = $previousDb
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    Pop-Location
}
