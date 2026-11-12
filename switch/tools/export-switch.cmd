@echo off
REM TASK-174: refresh export/switch-gpu, the single player folder (CoreGS GPU renderer is the default).
setlocal EnableExtensions
set "SWITCH_ROOT=%~dp0.."
set "REPO_ROOT=%SWITCH_ROOT%\.."
set "EXPORT_GPU=%REPO_ROOT%\export\switch-gpu"
set "NRO=%SWITCH_ROOT%\build\gt4recomp.nro"
set "ELF=%REPO_ROOT%\private\switch-ee\core.elf"

if not exist "%NRO%" (
  echo Missing "%NRO%"
  echo Build first: make -C switch require SWITCH_EE_GENERATED=../private/switch-ee/generated
  exit /b 1
)

mkdir "%EXPORT_GPU%" 2>nul
mkdir "%EXPORT_GPU%\saves" 2>nul
mkdir "%EXPORT_GPU%\shaders" 2>nul
copy /Y "%NRO%" "%EXPORT_GPU%\gt4recomp.nro" >nul
if exist "%ELF%" copy /Y "%ELF%" "%EXPORT_GPU%\core.elf" >nul
copy /Y "%SWITCH_ROOT%\README.md" "%EXPORT_GPU%\README.md" >nul
> "%EXPORT_GPU%\PUT_ISO_HERE.txt" echo Place your Gran Turismo 4 USA v2.00 plain .iso in this folder.
echo. > "%EXPORT_GPU%\saves\.keep"
echo. > "%EXPORT_GPU%\shaders\.keep"
REM CoreGS is the default now; the old opt-in marker is obsolete.
if exist "%EXPORT_GPU%\coregs.on" del /F /Q "%EXPORT_GPU%\coregs.on" >nul 2>&1

for %%F in ("%EXPORT_GPU%\gt4recomp.nro") do echo Exported: %%~fF  (%%~zF bytes)
echo Folder: %EXPORT_GPU%
exit /b 0
