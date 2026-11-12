@echo off
REM TASK-174: package == export. Keeps one clean SD-ready folder under export\switch.
call "%~dp0export-switch.cmd"
exit /b %ERRORLEVEL%
