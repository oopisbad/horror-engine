@echo off
setlocal
pushd "%~dp0"
if not exist "%~dp0build-windows\gorex_viewport.dll" (
    call "%~dp0build-windows.bat"
    if errorlevel 1 goto failed
)
python "%~dp0editor\map_editor.py" %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
popd
exit /b %result%
:failed
popd
pause
exit /b 1
