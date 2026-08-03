@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "ROMS_SRC=%~1"
if not defined ROMS_SRC set "ROMS_SRC=%ROOT%\roms"

set "DEVICE_ROOT=/sdcard/QuestRetroDepth"
set "DEVICE_ROMS_ROOT=%DEVICE_ROOT%/roms"

if not exist "%ROMS_SRC%" (
  echo ROM source folder not found:
  echo   %ROMS_SRC%
  echo.
  echo Put your legally owned ROMs under the repo roms folder, or pass another folder:
  echo   copy_roms.bat D:\MyRoms
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

echo Creating device ROM folder:
echo   %DEVICE_ROMS_ROOT%
"%ADB%" shell mkdir -p "%DEVICE_ROMS_ROOT%"
if errorlevel 1 exit /b 1

echo Copying ROM tree:
echo   from: %ROMS_SRC%
echo   to:   %DEVICE_ROMS_ROOT%
"%ADB%" push "%ROMS_SRC%\." "%DEVICE_ROMS_ROOT%/"
if errorlevel 1 (
  echo Failed to copy ROMs.
  exit /b 1
)

echo.
echo ROM copy complete.
exit /b 0
