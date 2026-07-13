@echo off
REM Mira Agent - chay nen tren may nha. Tu restart neu crash.
title Mira Agent
cd /d "%~dp0.."

:loop
python tools\agent.py
echo.
echo   Agent thoat (exit %errorlevel%) - khoi dong lai sau 5s...
timeout /t 5 /nobreak >nul
goto loop
