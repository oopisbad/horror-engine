@echo off
setlocal
cd /d "%~dp0"
call run-windows.bat --import-lab --play %*
