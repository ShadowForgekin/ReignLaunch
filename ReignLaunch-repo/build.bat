@echo off
chcp 65001 >nul
setlocal

REM ===== ReignLaunch build script =====
REM Requires MinGW-w64 GCC 8+ and windres in PATH

cd /d "%~dp0src"

where windres >nul 2>nul
if errorlevel 1 (
    echo [ERROR] windres not found. Add the MinGW-w64 bin folder to PATH first.
    pause
    exit /b 1
)

if not exist authlib-injector.jar (
    echo [ERROR] src\authlib-injector.jar missing
    echo         Download from https://github.com/yushijinhun/authlib-injector/releases
    echo         and put the jar into src\ named authlib-injector.jar
    pause
    exit /b 1
)

echo [1/2] compiling resources (icon + embedded jar)...
windres res.rc -O coff -o res.o
if errorlevel 1 goto fail

echo [2/2] compiling executable...
g++ -std=c++17 -O2 -mwindows -static -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -o ReignLaunch.exe ReignLaunch.cpp res.o -lcomctl32 -lole32 -lshell32 -lgdi32 -luuid -ladvapi32 -lwinhttp -lshlwapi -lcomdlg32
if errorlevel 1 goto fail

echo.
echo Build OK: src\ReignLaunch.exe
pause
exit /b 0

:fail
echo.
echo Build FAILED
pause
exit /b 1
