@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

call "%ROOT%\install_apk.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
call "%ROOT%\copy_roms.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
echo APK and ROM copy complete.
exit /b 0
