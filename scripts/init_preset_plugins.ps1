$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (Get-Command python -ErrorAction SilentlyContinue) {
    python "$ScriptDir\init_preset_plugins.py"
} elseif (Get-Command py -ErrorAction SilentlyContinue) {
    py "$ScriptDir\init_preset_plugins.py"
} else {
    Write-Error "Python not found. Please ensure Python is installed and added to PATH."
}
