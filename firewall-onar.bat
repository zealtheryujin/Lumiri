@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0firewall-onar.ps1"
if errorlevel 1 (
    echo Guvenlik duvari ayarlanamadi.
    pause
    exit /b 1
)
pause
