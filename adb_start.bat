@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "SDK=%LOCALAPPDATA%\Android\Sdk"
set "ADB=%SDK%\platform-tools\adb.exe"
set "PACKAGE_NAME=com.retrodepth.questretrodepth.debug"
set "ACTIVITY=com.retrodepth.questretrodepth.QuestVrActivity"

call "%ROOT%\adb_connect_wifi.bat"
if errorlevel 1 (
  echo WiFi connection failed. Trying USB...
)

timeout /t 2 /nobreak >nul

echo Checking for connected Quest devices...
"%ADB%" start-server >nul 2>nul
set "DEVICE_FOUND="
for /f "skip=1 tokens=1,2" %%A in ('"%ADB%" devices') do (
  if "%%B"=="device" set "DEVICE_FOUND=1"
)

if not defined DEVICE_FOUND (
  echo No Quest device detected over adb.
  exit /b 1
)

echo.
echo Starting QuestRetroDepth on Quest...
"%ADB%" shell am start -n %PACKAGE_NAME%/%ACTIVITY%