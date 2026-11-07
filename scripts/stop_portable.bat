@echo off
chcp 65001 >nul
title 注销人人输入法 (Personal IME)

echo [提示] 正在停止后台服务并注销输入法驱动...
taskkill /f /im ime-service.exe >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"\"%~dp0scripts\unregister.ps1\"\"' -Verb RunAs -Wait"
echo [完成] 输入法驱动已注销，服务已停止。可直接删除此文件夹。
pause

