@echo off
chcp 65001 >nul
cd /d "%~dp0"

for /f "tokens=1,2 delims=," %%a in ('powershell -NoProfile -Command "Get-Date -Format 'MM-dd,HH-mm'"') do set "msg=%%a %%b"

echo commit message: %msg%
git add .
git commit -m "%msg%"
git push -u origin main

pause
