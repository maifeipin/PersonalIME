<#
.SYNOPSIS
    Cleans build artifacts, test sandbox files, and temporary outputs.
.DESCRIPTION
    Safely removes transient directories and files generated during build and testing.
    Leaves source code, configuration, dictionaries, and documentation intact.
.PARAMETER All
    Removes build/ and out/ artifacts in addition to temporary files.
.PARAMETER TempOnly
    Removes only temporary test databases and residual test directories (keeps build/ and out/).
#>

param(
    [switch]$All = $true,
    [switch]$TempOnly
)

$ErrorActionPreference = "SilentlyContinue"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptRoot

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Personal IME: Workspace Cleaning Utility" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$deletedCount = 0

function Remove-Target([string]$Path, [string]$Description) {
    if (Test-Path $Path) {
        Remove-Item -Path $Path -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "  [-] Removed: $Description ($Path)" -ForegroundColor Yellow
        $script:deletedCount++
    }
}

# 1. Clean root temporary test directories and residual files
Write-Host "`n[1/3] Scanning for test temporary directories and databases..." -ForegroundColor Cyan
Get-ChildItem -Path $ProjectRoot -Directory -Filter "receipt-*" | ForEach-Object {
    Remove-Target $_.FullName "Residual test receipt directory"
}
Get-ChildItem -Path $ProjectRoot -Directory -Filter "handover-*" | ForEach-Object {
    Remove-Target $_.FullName "Residual test handover directory"
}
Get-ChildItem -Path $ProjectRoot -Directory -Filter "test_r2_dicts*" | ForEach-Object {
    Remove-Target $_.FullName "Residual test dictionary directory"
}

# Clean root temporary SQLite databases and journal files
$rootDbPatterns = @("*.db", "*.db-shm", "*.db-wal", "*.db-journal", "test_*.json", "test_service.json")
foreach ($pat in $rootDbPatterns) {
    Get-ChildItem -Path $ProjectRoot -File -Filter $pat | ForEach-Object {
        Remove-Target $_.FullName "Residual test database / journal"
    }
}

# Clean loose dll in root
if (Test-Path (Join-Path $ProjectRoot "PersonalIME.dll")) {
    Remove-Target (Join-Path $ProjectRoot "PersonalIME.dll") "Loose DLL in root"
}

# 2. Clean C# bin and obj directories
Write-Host "`n[2/3] Cleaning .NET bin and obj directories..." -ForegroundColor Cyan
Get-ChildItem -Path (Join-Path $ProjectRoot "src\manager") -Directory -Recurse -Include "bin", "obj" | ForEach-Object {
    Remove-Target $_.FullName "C# build directory"
}

# 3. Clean build/ and out/ if -All or not -TempOnly
if (-not $TempOnly) {
    Write-Host "`n[3/3] Cleaning build and output directories..." -ForegroundColor Cyan
    Remove-Target (Join-Path $ProjectRoot "build") "CMake build directory"
    Remove-Target (Join-Path $ProjectRoot "out") "Output & release packaging directory"
} else {
    Write-Host "`n[3/3] Preserved build/ and out/ (-TempOnly mode)." -ForegroundColor Green
}

Write-Host "`n==========================================================" -ForegroundColor Green
Write-Host "  Cleanup completed! ($deletedCount targets removed)" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
