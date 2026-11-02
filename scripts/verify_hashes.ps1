$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$files = @(
    "build\Release\PersonalIME.dll",
    "build\Release\ime-service.exe",
    "build\Release\test_composition_lifecycle.exe",
    "build\Release\test_ipc_runner.exe",
    "build\Release\test_candidate_window.exe",
    "build\Release\test_com_lifecycle.exe",
    "src\tsf_client\edit_session.h",
    "src\tsf_client\ipc_client.cpp",
    "src\tsf_client\ipc_client.h",
    "src\tsf_client\text_input_processor.cpp",
    "src\tsf_client\text_input_processor.h"
)

$results = foreach ($f in $files) {
    $fullPath = Join-Path $projectRoot $f
    if (Test-Path $fullPath) {
        $hash = (Get-FileHash -Algorithm SHA256 -Path $fullPath).Hash
        [PSCustomObject]@{
            File = $f
            SHA256 = $hash
        }
    } else {
        [PSCustomObject]@{
            File = $f
            SHA256 = "NOT_FOUND"
        }
    }
}

$results | Format-Table -Property File, SHA256 -AutoSize

