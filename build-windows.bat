@echo off
setlocal
cd /d "%~dp0"
set "PATH=%~dp0tools\w64devkit\bin;%PATH%"
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 exit /b 1
cmake --build build-windows --parallel 4

