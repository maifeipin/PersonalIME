<#
.SYNOPSIS
    Verify Portable Package Dry-Run in Non-Project Directory with Chinese & Spaces
#>
$ErrorActionPreference = "Stop"

$folderName = "Personal IME " + [char]0x7eff + [char]0x8272 + [char]0x5305 + " Space " + [Guid]::NewGuid().ToString("N")
$testRoot = Join-Path $env:TEMP $folderName
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$scriptsDir = Join-Path $testRoot "scripts"
New-Item -ItemType Directory -Path $scriptsDir -Force | Out-Null

$projectRoot = Split-Path -Parent $PSScriptRoot
Copy-Item (Join-Path $projectRoot "build\Release\PersonalIME.dll") -Destination $testRoot
Copy-Item (Join-Path $projectRoot "scripts\register.ps1") -Destination $scriptsDir
Copy-Item (Join-Path $projectRoot "scripts\unregister.ps1") -Destination $scriptsDir

try {
    Write-Host "Running register.ps1 -DryRun in: $scriptsDir" -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptsDir "register.ps1") -DryRun
    $regExit = $LASTEXITCODE

    Write-Host "`nRunning unregister.ps1 -DryRun in: $scriptsDir" -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptsDir "unregister.ps1") -DryRun
    $unregExit = $LASTEXITCODE

    Write-Host "`nVerification result: register ExitCode=$regExit, unregister ExitCode=$unregExit" -ForegroundColor Yellow
    if ($regExit -ne 0) { throw "register.ps1 -DryRun failed with exit code $regExit" }
    if ($unregExit -ne 0) { throw "unregister.ps1 -DryRun failed with exit code $unregExit" }

    Write-Host "==> [PASS] Portable registration script successfully resolved root DLL in path with spaces & Chinese!" -ForegroundColor Green
} finally {
    Remove-Item -Path $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
