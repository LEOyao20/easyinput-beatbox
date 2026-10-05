@echo off
rem ENCODING: GBK on purpose (Chinese messages + cmd.exe codepage 936).
rem Do not re-save this file as UTF-8 -- cmd.exe reads .cmd by byte offset
rem and a later codepage change makes it resume mid-line and run garbage.
setlocal
title EasyInput Beatbox - Web 停止

set "PORT=5173"

echo ============================================
echo   EasyInput Beatbox - Web 停止
echo ============================================
echo.

set "FOUND="
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":%PORT%" ^| findstr "LISTENING"') do (
    if not defined FOUND set "FOUND=1"
    echo [信息] 终止进程 PID %%P ...
    taskkill /F /T /PID %%P >nul 2>&1
)

if not defined FOUND (
    echo [提示] 端口 %PORT% 上没有运行中的服务，无需停止。
    echo.
    ping -n 3 127.0.0.1 >nul
    exit /b 0
)

echo.
echo [成功] Web 服务已停止，浏览器里的页面现在可以关掉了。
echo.
ping -n 3 127.0.0.1 >nul
exit /b 0
