@echo off
setlocal EnableExtensions

set "FOUND_ADB="

if defined ANDROID_HOME if exist "%ANDROID_HOME%\platform-tools\adb.exe" set "FOUND_ADB=%ANDROID_HOME%\platform-tools\adb.exe"
if not defined FOUND_ADB if defined ANDROID_SDK_ROOT if exist "%ANDROID_SDK_ROOT%\platform-tools\adb.exe" set "FOUND_ADB=%ANDROID_SDK_ROOT%\platform-tools\adb.exe"
if not defined FOUND_ADB if exist "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" set "FOUND_ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
if not defined FOUND_ADB (
  for /f "delims=" %%A in ('where adb.exe 2^>nul') do if not defined FOUND_ADB set "FOUND_ADB=%%A"
)

if not defined FOUND_ADB (
  echo adb.exe was not found.
  echo Install Android Platform Tools, or set ANDROID_HOME or ANDROID_SDK_ROOT.
  exit /b 1
)

endlocal & set "ADB=%FOUND_ADB%"
exit /b 0
