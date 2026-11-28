@echo off
rem Builds ArcticUsbCreator.exe with llvm-mingw (a native Windows toolchain).
rem LLVM_MINGW points at its folder; the default is where it lives on the dev PC.
setlocal
if "%LLVM_MINGW%"=="" set LLVM_MINGW=F:\apps\llvm-mingw-20261104-ucrt-x86_64
set PATH=%LLVM_MINGW%\bin;%PATH%
cd /d "%~dp0"
if not exist out mkdir out
x86_64-w64-mingw32-windres app.rc -O coff -o out\app.res || exit /b 1
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -municode -mwindows -static ^
  -fno-exceptions -fno-rtti ^
  main.cpp out\app.res -o out\ArcticUsbCreator.exe ^
  -ld2d1 -ldwrite -lwinhttp -lbcrypt -lcomctl32 -lcomdlg32 -lshell32 -lole32 -ldwmapi -luuid -ladvapi32 -lgdi32 -lsetupapi || exit /b 1
echo built out\ArcticUsbCreator.exe
