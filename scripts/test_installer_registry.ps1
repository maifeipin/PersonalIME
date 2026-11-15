<#
.SYNOPSIS
Compiles and runs the production Inno registry snapshot helpers in isolation.
.DESCRIPTION
Writes only a disposable HKCU\Software\PersonalIMEInstallerTests child key.
Does not install PersonalIME or modify COM/TSF registration. The harness exits
from InitializeSetup before entering the installation phase.
#>
param([string]$IsccPath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $IsccPath) {
    foreach ($candidate in @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe")) {
        if (Test-Path -LiteralPath $candidate) { $IsccPath = $candidate; break }
    }
    if (-not $IsccPath) { $IsccPath = (Get-Command ISCC.exe -ErrorAction Stop).Source }
}
$outputDir = Join-Path $projectRoot 'out\installer-tests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$runId = [guid]::NewGuid().ToString('N')
$resultFile = Join-Path $outputDir "result-$runId.txt"
$logFile = Join-Path $outputDir "harness-$runId.log"
& $IsccPath /Q (Join-Path $projectRoot 'tests\test_installer_registry.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno regression harness compilation failed' }
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', "/RESULTFILE=`"$resultFile`"", "/LOG=`"$logFile`"")
$harness = Start-Process -FilePath (Join-Path $outputDir 'test_installer_registry.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
if (-not $harness.WaitForExit(30000)) {
    $harness.Kill()
    throw "Registry harness timed out; see $logFile"
}
# InitializeSetup intentionally returns False, so the result marker is the test
# verdict. A unique marker prevents a prior successful run from masking a crash.
if (-not (Test-Path -LiteralPath $resultFile)) { throw "Harness produced no result; see $logFile" }
$result = Get-Content -LiteralPath $resultFile -Raw
if (-not $result.StartsWith('PASS:')) { throw "$result (log: $logFile)" }
Write-Output $result.Trim()
