$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$files = @(
    "build\Release\PersonalIME.dll",
    "build\Release\ime-service.exe",
    "build\Release\ime-cli.exe",
    "build\Release\test_ime_core.exe",
    "build\Release\test_composition_lifecycle.exe",
    "build\Release\test_ipc_runner.exe",
    "build\Release\test_candidate_window.exe",
    "build\Release\test_com_lifecycle.exe",
    "src\core\engine\ime_engine.h",
    "src\core\engine\ime_engine.cpp",
    "src\core\storage\sqlite_db.h",
    "src\core\storage\sqlite_db.cpp",
    "src\core\storage\entry_repository.h",
    "src\core\storage\entry_repository.cpp",
    "src\core\ranking\ranker.h",
    "src\core\ranking\ranker.cpp",
    "src\core\encoding\pinyin_segmenter.h",
    "src\core\encoding\pinyin_segmenter.cpp",
    "data\dictionaries\wubi86.dict",
    "data\dictionaries\pinyin.dict"
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

