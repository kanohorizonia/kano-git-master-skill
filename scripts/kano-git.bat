@echo off
REM kano-git launcher (Windows CMD)
REM Delegates to Bash launcher

setlocal
set "SCRIPT_DIR=%~dp0"
set "BASH_EXE=C:\Program Files\Git\bin\bash.exe"
if not exist "%BASH_EXE%" (
  for %%I in (bash.exe) do set "BASH_EXE=%%~$PATH:I"
)
if "%BASH_EXE%"=="" (
  echo Error: bash.exe not found. Install Git for Windows or add bash to PATH. 1>&2
  exit /b 1
)

"%BASH_EXE%" "%SCRIPT_DIR%kano-git" %*
exit /b %ERRORLEVEL%
