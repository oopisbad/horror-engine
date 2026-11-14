@echo off
setlocal
cd /d "%~dp0"
call run-import-lab.bat --w 1920 --h 1080 --fps-cap 60 %*
