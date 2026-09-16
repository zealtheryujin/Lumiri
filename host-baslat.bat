@echo off
cd /d "%~dp0"
lumiri-host.exe
set "taskHostExit=%errorlevel%"
echo.
echo Host kapandi. Cikis kodu: %taskHostExit%
pause
exit /b %taskHostExit%
