@echo off
REM kog — short alias for kano-git (Windows CMD)
setlocal
set "SCRIPT_DIR=%~dp0"
python "%SCRIPT_DIR%kog" %*
exit /b %ERRORLEVEL%
