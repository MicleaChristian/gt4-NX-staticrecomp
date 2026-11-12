@echo off
REM TASK-174: build gt4recomp.nro when DevKitPro is installed.
setlocal
if "%DEVKITPRO%"=="" (
  if exist "C:\devkitPro\libnx" set "DEVKITPRO=C:\devkitPro"
)
if "%DEVKITPRO%"=="" (
  echo DEVKITPRO is not set. Install DevKitPro switch-dev and set DEVKITPRO.
  echo See switch\README.md
  exit /b 1
)
where make >nul 2>&1
if errorlevel 1 (
  echo GNU make not on PATH. Use the DevKitPro MSYS2 shell, or add make to PATH.
  exit /b 1
)
cd /d "%~dp0.."
make require
exit /b %ERRORLEVEL%
