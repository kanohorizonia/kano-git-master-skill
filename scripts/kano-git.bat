@echo off
REM kano-git launcher (Windows CMD)
REM Delegates to Python launcher

setlocal
set "SCRIPT_DIR=%~dp0"
python "%SCRIPT_DIR%kano-git" %*
exit /b %ERRORLEVEL%
