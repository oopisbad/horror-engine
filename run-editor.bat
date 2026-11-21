@echo off
setlocal
pushd "%~dp0"
python "%~dp0editor\map_editor.py" %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
popd
exit /b %result%
