@echo off
cd /d "%~dp0"
git add -A
git commit -m "%*"
git remote get-url origin >nul 2>&1 || gh repo create lumiri --private --source . --remote origin
git push -u origin HEAD
pause
