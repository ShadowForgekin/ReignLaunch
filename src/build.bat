@echo off
setlocal
set MINGW=D:\winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r2_2\mingw64\bin
set GCC=%MINGW%\g++.exe
set CC=%MINGW%\gcc.exe
set WINDRES=%MINGW%\windres.exe
set COMMON=-std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -I. -IFL

echo [1/4] windres
"%WINDRES%" res.rc -O coff -o res.o || goto :err

echo [2/4] miniz.c
"%CC%" -O2 -c miniz.c -o miniz.o || goto :err

echo [3/4] ReignLaunch.cpp
"%GCC%" %COMMON% -c -o ReignLaunch.o ReignLaunch.cpp || goto :err

echo [4/4] link
"%GCC%" %COMMON% -mwindows -static -static-libgcc -static-libstdc++ -o ReignLaunch.exe ReignLaunch.o miniz.o res.o -Wl,--start-group libfltk.a -Wl,--end-group -lwinhttp -lshell32 -ladvapi32 -lole32 -luuid -lgdi32 -lcomctl32 -loleaut32 -lshlwapi -lcomdlg32 -lgdiplus -lwinspool -lws2_32 || goto :err

echo.
echo OK -^> ReignLaunch.exe
goto :eof

:err
echo BUILD FAILED
exit /b 1