@echo off
REM TASK-174: refresh export/switch (soft GS) and export/switch-gpu (CoreGS WritebackDk).
REM Same NRO binary; CoreGS is selected by coregs.on next to the NRO.
setlocal EnableExtensions
set "SWITCH_ROOT=%~dp0.."
set "REPO_ROOT=%SWITCH_ROOT%\.."
set "EXPORT_SOFT=%REPO_ROOT%\export\switch"
set "EXPORT_GPU=%REPO_ROOT%\export\switch-gpu"
set "NRO=%SWITCH_ROOT%\build\gt4recomp.nro"
set "ELF=%REPO_ROOT%\private\switch-ee\core.elf"

if not exist "%NRO%" (
  echo Missing "%NRO%"
  echo Build first: make -C switch require SWITCH_EE_GENERATED=../private/switch-ee/generated
  exit /b 1
)

REM ---- Soft (donor CPU GS) ----
mkdir "%EXPORT_SOFT%" 2>nul
mkdir "%EXPORT_SOFT%\saves" 2>nul
mkdir "%EXPORT_SOFT%\shaders" 2>nul
copy /Y "%NRO%" "%EXPORT_SOFT%\gt4recomp.nro" >nul
if exist "%ELF%" copy /Y "%ELF%" "%EXPORT_SOFT%\core.elf" >nul
copy /Y "%SWITCH_ROOT%\README.md" "%EXPORT_SOFT%\README.md" >nul
if exist "%EXPORT_SOFT%\PERF.md" (
  rem keep in-tree PERF.md
) else if exist "%EXPORT_GPU%\PERF.md" (
  copy /Y "%EXPORT_GPU%\PERF.md" "%EXPORT_SOFT%\PERF.md" >nul
)
> "%EXPORT_SOFT%\PUT_ISO_HERE.txt" echo Place your Gran Turismo 4 USA v2.00 plain .iso in this folder.
echo. > "%EXPORT_SOFT%\saves\.keep"
echo. > "%EXPORT_SOFT%\shaders\.keep"
if exist "%EXPORT_SOFT%\coregs.on" del /F /Q "%EXPORT_SOFT%\coregs.on" >nul 2>&1
if exist "%EXPORT_SOFT%\THIS_IS_GPU.txt" del /F /Q "%EXPORT_SOFT%\THIS_IS_GPU.txt" >nul 2>&1
> "%EXPORT_SOFT%\THIS_IS_SOFT.txt" echo Donor CPU GS + deko3d present. No coregs.on.

REM ---- GPU (CoreGS WritebackDk) ----
mkdir "%EXPORT_GPU%" 2>nul
mkdir "%EXPORT_GPU%\saves" 2>nul
mkdir "%EXPORT_GPU%\shaders" 2>nul
copy /Y "%NRO%" "%EXPORT_GPU%\gt4recomp.nro" >nul
if exist "%ELF%" copy /Y "%ELF%" "%EXPORT_GPU%\core.elf" >nul
copy /Y "%SWITCH_ROOT%\README.md" "%EXPORT_GPU%\README.md" >nul
> "%EXPORT_GPU%\PUT_ISO_HERE.txt" echo Place your Gran Turismo 4 USA v2.00 plain .iso in this folder.
echo. > "%EXPORT_GPU%\saves\.keep"
echo. > "%EXPORT_GPU%\shaders\.keep"
type nul > "%EXPORT_GPU%\coregs.on"
> "%EXPORT_GPU%\THIS_IS_GPU.txt" echo CoreGS BackendDk + WritebackDk GPU path. coregs.on is present.
if exist "%EXPORT_GPU%\THIS_IS_SOFT.txt" del /F /Q "%EXPORT_GPU%\THIS_IS_SOFT.txt" >nul 2>&1

for %%F in ("%EXPORT_SOFT%\gt4recomp.nro") do echo Exported [soft]: %%~fF  (%%~zF bytes)
for %%F in ("%EXPORT_GPU%\gt4recomp.nro") do echo Exported [gpu]:  %%~fF  (%%~zF bytes)
echo Soft folder: %EXPORT_SOFT%
echo GPU folder:  %EXPORT_GPU%
exit /b 0
