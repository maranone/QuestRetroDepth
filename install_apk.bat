@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "APK=%~1"
if not defined APK set "APK=%ROOT%\apk\QuestRetroDepth-debug.apk"

if not exist "%APK%" (
  echo APK not found:
  echo   %APK%
  echo.
  echo Run build.bat first, or pass an APK path:
  echo   install_apk.bat path\to\QuestRetroDepth.apk
  exit /b 1
)

call "%ROOT%\tools_adb.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

echo Checking for connected Quest devices...
"%ADB%" start-server >nul 2>nul
set "DEVICE_FOUND="
for /f "skip=1 tokens=1,2" %%A in ('"%ADB%" devices') do (
  if "%%B"=="device" set "DEVICE_FOUND=1"
)

if not defined DEVICE_FOUND (
  echo No Quest device detected over adb.
  echo Connect the headset with USB and allow USB debugging, then run this again.
  exit /b 1
)

echo Installing APK:
echo   %APK%
"%ADB%" install -r "%APK%"
if errorlevel 1 (
  echo adb install failed.
  exit /b 1
)

echo.
echo APK install complete.
exit /b 0
