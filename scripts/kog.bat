@echo off
setlocal

set "SCRIPT_DIR=%~dp0"

call "%SCRIPT_DIR%kano-git.bat" %*
exit /b %ERRORLEVEL%
