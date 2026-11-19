@echo off
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    python "%~dp0setup.py" %*
    goto :end
)

where py >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    py -3 "%~dp0setup.py" %*
    goto :end
)

echo Error: Python 3 is required to run the LocaLeShop setup wizard.
echo Install Python 3.9+ from https://www.python.org/ and ensure "Add Python to PATH" is checked.

:end
echo.
pause
