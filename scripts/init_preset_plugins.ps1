# PersonalIME 预置插件与数据幂等初始化
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (Get-Command python -ErrorAction SilentlyContinue) {
    python "$ScriptDir\init_preset_plugins.py"
} elseif (Get-Command py -ErrorAction SilentlyContinue) {
    py "$ScriptDir\init_preset_plugins.py"
} else {
    Write-Error "未找到 Python，请确保已安装 Python 并加入系统 PATH。"
}
