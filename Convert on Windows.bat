@echo off
rem Double-click this to open the converter window.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 tools\converter_gui.py & goto :eof)
where python >nul 2>nul && (python tools\converter_gui.py & goto :eof)
echo Python 3 is not installed. Get it from https://www.python.org/downloads/
pause
