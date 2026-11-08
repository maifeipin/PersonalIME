@echo off
python "%~dp0init_preset_plugins.py"
if %ERRORLEVEL% neq 0 (
    py "%~dp0init_preset_plugins.py"
)
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to run python script.
)
echo %cmdcmdline% | find /i "%~0" >nul
if %ERRORLEVEL% equ 0 pause
