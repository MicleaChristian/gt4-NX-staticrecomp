@echo off
rem GT4Recomp NX 0.2.0: makes core.elf from YOUR OWN Gran Turismo 4 (USA, v2.00) disc image.
rem Usage: drag your .iso onto this file, or run: make-core-elf.cmd "C:\path\to\game.iso"
rem Needs Python 3 (python.org). Nothing is downloaded; the game data never leaves your PC.
setlocal
if "%~1"=="" (
  echo Drag your Gran Turismo 4 USA v2.00 .iso onto this file.
  pause
  exit /b 2
)
set HERE=%~dp0
set WORK=%HERE%work
if not exist "%WORK%" mkdir "%WORK%"
python -I "%HERE%disc.py" extract "%~1" "%WORK%" CORE.GT4
if errorlevel 1 goto fail
python -I "%HERE%gt4_core.py" "%WORK%\CORE.GT4" "%HERE%core.elf"
if errorlevel 1 goto fail
del "%WORK%\CORE.GT4" 2>nul
rmdir "%WORK%" 2>nul
echo.
echo Done: %HERE%core.elf
echo Copy core.elf to the SD card folder switch\GT4, next to gt4recomp.nro.
pause
exit /b 0
:fail
echo.
echo Failed. Is Python 3 installed, and is this the USA v2.00 disc image?
pause
exit /b 1
