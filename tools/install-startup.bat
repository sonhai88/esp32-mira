@echo off
REM Chay 1 lan duy nhat: cai Mira Agent tu khoi dong cung Windows.
setlocal
set "TARGET=%~dp0mira-agent.bat"
set "WORKDIR=%~dp0.."

powershell -NoProfile -Command ^
  "$sh = New-Object -ComObject WScript.Shell;" ^
  "$lnk = $sh.CreateShortcut([Environment]::GetFolderPath('Startup') + '\Mira Agent.lnk');" ^
  "$lnk.TargetPath = '%TARGET%';" ^
  "$lnk.WorkingDirectory = '%WORKDIR%';" ^
  "$lnk.WindowStyle = 7;" ^
  "$lnk.Save()"

if %errorlevel% neq 0 (
  echo   [LOI] Khong tao duoc shortcut.
) else (
  echo   [OK] Da cai. Tu gio bat may la Mira Agent tu chay.
  echo   Go cai: xoa "Mira Agent.lnk" trong thu muc shell:startup
)
echo.
echo   Dang chay agent ngay bay gio...
timeout /t 2 /nobreak >nul
call "%TARGET%"
