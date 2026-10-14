@echo off
REM Launches fetch-deps.sh inside the MSYS2 bash that ships with devkitPro.
setlocal
if "%DEVKITPRO%"=="" set "DEVKITPRO=C:\devkitPro"
set "BASH=%DEVKITPRO%\msys2\usr\bin\bash.exe"
if not exist "%BASH%" set "BASH=C:\devkitPro\msys2\usr\bin\bash.exe"
if not exist "%BASH%" (
  echo Open MSYS2 from the Start menu ^(C:\devkitPro\msys2\msys2.exe^)
  echo then run:  ./fetch-deps.sh
  exit /b 1
)
"%BASH%" -lc "cd \"$(cygpath '%CD%')\" && ./fetch-deps.sh"
exit /b %ERRORLEVEL%
