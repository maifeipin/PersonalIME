@echo off
chcp 65001 >nul
title PersonalIME 预置插件与数据幂等初始化

echo ========================================================
echo   PersonalIME: 预置插件与数据幂等初始化
echo ========================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "PY_SCRIPT=%SCRIPT_DIR%init_preset_plugins.py"
set "SQL_FILE=%SCRIPT_DIR%init_preset_plugins.sql"
set "DB_PATH=%APPDATA%\PersonalIME\user_dict.db"

where python >nul 2>nul
if %ERRORLEVEL% equ 0 (
    python "%PY_SCRIPT%"
    goto check_result
)

where py >nul 2>nul
if %ERRORLEVEL% equ 0 (
    py "%PY_SCRIPT%"
    goto check_result
)

where sqlite3 >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [RUN] 正在通过 sqlite3.exe 执行 SQL...
    if not exist "%APPDATA%\PersonalIME" mkdir "%APPDATA%\PersonalIME"
    sqlite3 "%DB_PATH%" < "%SQL_FILE%"
    if %ERRORLEVEL% equ 0 (
        echo [SUCCESS] 数据库脚本执行完成！
        goto check_result
    )
)

echo [ERROR] 未找到 Python 或 sqlite3.exe，请确保系统已安装 Python 并在 PATH 中。

:check_result
echo.
echo ========================================================
echo   操作完成。按任意键退出...
echo ========================================================
pause >nul
