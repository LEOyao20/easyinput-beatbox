@echo off
rem ENCODING: GBK on purpose (Chinese messages + cmd.exe codepage 936).
rem Do not re-save this file as UTF-8 -- cmd.exe reads .cmd by byte offset
rem and a later codepage change makes it resume mid-line and run garbage.
setlocal
title EasyInput Beatbox - Web 启动

set "ROOT=%~dp0"
set "APPDIR=%ROOT%app"
set "VITE=%APPDIR%\node_modules\vite\bin\vite.js"
set "HOST=127.0.0.1"
set "PORT=5173"
set "URL=http://%HOST%:%PORT%/"

echo ============================================
echo   EasyInput Beatbox - Web 控制台
echo ============================================
echo.

if not exist "%VITE%" (
    echo [错误] 找不到 Vite：
    echo        %VITE%
    echo        请先在 app 目录执行一次 pnpm install
    echo.
    pause
    exit /b 1
)

call :FindPid
if defined WEBPID (
    echo [提示] 服务已在运行 ^(PID %WEBPID%^)，直接打开浏览器。
    start "" "%URL%"
    exit /b 0
)

set "NODE="
for /f "delims=" %%I in ('where node 2^>nul') do if not defined NODE set "NODE=%%I"
if not defined NODE (
    for /d %%D in ("%USERPROFILE%\.workbuddy\binaries\node\versions\*") do (
        if not defined NODE if exist "%%D\node.exe" set "NODE=%%D\node.exe"
    )
)
if not defined NODE (
    echo [错误] 找不到 node.exe。
    echo        请安装 Node.js，或确认 WorkBuddy 运行时目录存在：
    echo        %USERPROFILE%\.workbuddy\binaries\node\versions\
    echo.
    pause
    exit /b 1
)

echo [信息] Node : %NODE%
echo [信息] 地址 : %HOST%:%PORT%
echo [信息] 正在启动服务 ...
echo.

rem 必须把 host 钉死成 127.0.0.1：Vite 默认的 localhost 会随系统解析顺序
rem 绑到 IPv4 或 IPv6，绑错时浏览器就打不开。
rem 服务托管在一个独立的最小化窗口里，关掉它或跑 stop-web.cmd 都能停止。
cd /d "%APPDIR%"
start "EasyInput Beatbox Web" /min cmd /k ""%NODE%" "%VITE%" --host %HOST% --port %PORT% --strictPort"

set /a TRIES=0
:WaitLoop
call :FindPid
if defined WEBPID goto Ready
set /a TRIES+=1
if %TRIES% GEQ 30 goto Failed
ping -n 2 127.0.0.1 >nul
goto WaitLoop

:Ready
echo [成功] Web 服务已启动 ^(PID %WEBPID%^)
echo        地址：%URL%
echo.
start "" "%URL%"
echo 提示：服务在最小化的 EasyInput Beatbox Web 窗口里运行。
echo       停止请运行 stop-web.cmd，或直接关掉那个窗口。
exit /b 0

:Failed
echo [失败] 等待 30 秒后服务仍未就绪。
echo        请还原最小化的 EasyInput Beatbox Web 窗口查看错误信息。
echo.
pause
exit /b 1

:FindPid
set "WEBPID="
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":%PORT%" ^| findstr "LISTENING"') do if not defined WEBPID set "WEBPID=%%P"
exit /b 0
