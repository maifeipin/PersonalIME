@echo off
chcp 65001 >nul
title 启动人人输入法 (Personal IME)

echo ==========================================================
echo   人人 (Personal IME) 便携免安装版
echo ==========================================================

:: 检查是否已注册 COM TSF CLSID
reg query "HKCR\CLSID\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}" >nul 2>&1
if %errorlevel% neq 0 (
    echo [提示] 首次运行需要向系统注册输入法驱动，即将请求管理员权限...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"\"%~dp0scripts\register.ps1\"\"' -Verb RunAs -Wait"
)

:: 启动后台输入服务
echo [启动] 正在启动后台输入服务...
start "" "%~dp0ime-service.exe"

echo [完成] 输入法服务已在后台运行！请在语言栏切换到“人人 (Personal IME)”即可打字。
timeout /t 3 >nul

