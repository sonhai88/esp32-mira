@echo off
REM Mira Agent - chay nen. Crash thi tu bat lai.
title Mira Agent
cd /d "%~dp0"

:loop
python "%~dp0agent.py"
echo.
echo   Agent thoat (exit %errorlevel%) - khoi dong lai sau 5s...
timeout /t 5 /nobreak >nul
goto loop
