@echo off
REM ============================================================
REM  Mira Agent - cai dat tren may nha. Chay 1 lan duy nhat.
REM  Khong can git, khong can PlatformIO, khong can source code.
REM ============================================================
setlocal
set "APP=%LOCALAPPDATA%\Mira"
set "RAW=https://raw.githubusercontent.com/sonhai88/esp32-mira/main/tools"

echo.
echo   === Cai dat Mira Agent ===
echo.

REM --- 1. Kiem tra Python ---
python --version >nul 2>&1
if errorlevel 1 (
  echo   [LOI] Chua co Python.
  echo   Tai tai: https://python.org/downloads  ^(nho tick "Add Python to PATH"^)
  pause
  exit /b 1
)
echo   [OK] Python co san

REM --- 2. Cai thu vien ---
echo   [..] Cai thu vien ^(pyserial, requests, esptool^)...
python -m pip install --quiet --upgrade pyserial requests esptool
if errorlevel 1 (
  echo   [LOI] Cai thu vien that bai.
  pause
  exit /b 1
)
echo   [OK] Thu vien xong

REM --- 3. Tai agent ---
if not exist "%APP%" mkdir "%APP%"
echo   [..] Tai phan mem moi nhat...
curl -fsSL "%RAW%/agent.py"       -o "%APP%\agent.py"
curl -fsSL "%RAW%/mira-agent.bat" -o "%APP%\mira-agent.bat"
if not exist "%APP%\agent.py" (
  echo   [LOI] Tai agent that bai - kiem tra mang.
  pause
  exit /b 1
)
echo   [OK] Da cai vao %APP%

REM --- 4. Tu khoi dong cung Windows ---
powershell -NoProfile -Command ^
  "$sh = New-Object -ComObject WScript.Shell;" ^
  "$lnk = $sh.CreateShortcut([Environment]::GetFolderPath('Startup') + '\Mira Agent.lnk');" ^
  "$lnk.TargetPath = '%APP%\mira-agent.bat';" ^
  "$lnk.WorkingDirectory = '%APP%';" ^
  "$lnk.WindowStyle = 7;" ^
  "$lnk.Save()"
echo   [OK] Tu chay khi bat may

echo.
echo   === XONG ===
echo   Tu gio: chi can CAM DAY USB vao ESP32. Khong phai lam gi khac.
echo   Phan mem tu cap nhat, tu flash firmware khi duoc lenh tu xa.
echo.
timeout /t 3 /nobreak >nul
cd /d "%APP%"
call "%APP%\mira-agent.bat"
