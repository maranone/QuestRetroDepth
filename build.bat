@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%"

set "GRADLE_VERSION=8.7"
set "GRADLE_DIR=%ROOT%\.tools\gradle-%GRADLE_VERSION%"
set "GRADLE_CMD=%GRADLE_DIR%\bin\gradle.bat"
set "APK_SRC=%ROOT%\app\build\outputs\apk\debug\app-debug.apk"
set "APK_DST_DIR=%ROOT%\apk"
set "APK_DST=%APK_DST_DIR%\QuestRetroDepth-debug.apk"

if defined ANDROID_HOME set "ANDROID_SDK=%ANDROID_HOME%"
if not defined ANDROID_SDK if defined ANDROID_SDK_ROOT set "ANDROID_SDK=%ANDROID_SDK_ROOT%"
if not defined ANDROID_SDK set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"
if not exist "%ANDROID_SDK%" (
  echo Android SDK not found.
  echo Set ANDROID_HOME or ANDROID_SDK_ROOT to your Android SDK folder, then run this again.
  exit /b 1
)
set "ANDROID_HOME=%ANDROID_SDK%"

where java >nul 2>nul
if errorlevel 1 (
  echo Java was not found on PATH.
  echo Install JDK 21 or set JAVA_HOME and add %JAVA_HOME%\bin to PATH.
  exit /b 1
)

if exist "%ROOT%\gradlew.bat" (
  set "GRADLE_CMD=%ROOT%\gradlew.bat"
) else if not exist "%GRADLE_CMD%" (
  where gradle.bat >nul 2>nul
  if not errorlevel 1 (
    for /f "delims=" %%G in ('where gradle.bat 2^>nul') do if not defined GRADLE_CMD set "GRADLE_CMD=%%G"
  ) else (
    echo Gradle was not found. Downloading Gradle %GRADLE_VERSION% into .tools...
    mkdir "%ROOT%\.tools" 2>nul
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$zip='%ROOT%\.tools\gradle-%GRADLE_VERSION%-bin.zip'; Invoke-WebRequest -Uri 'https://services.gradle.org/distributions/gradle-%GRADLE_VERSION%-bin.zip' -OutFile $zip; Expand-Archive -LiteralPath $zip -DestinationPath '%ROOT%\.tools' -Force"
    if errorlevel 1 exit /b %ERRORLEVEL%
  )
)

echo Using project: %ROOT%
echo Using Android SDK: %ANDROID_SDK%
echo Using Gradle: %GRADLE_CMD%

call "%GRADLE_CMD%" --no-daemon assembleDebug
if errorlevel 1 exit /b %ERRORLEVEL%

if not exist "%APK_SRC%" (
  echo APK not found at "%APK_SRC%"
  exit /b 1
)

mkdir "%APK_DST_DIR%" 2>nul
copy /y "%APK_SRC%" "%APK_DST%" >nul

echo.
echo APK copied to:
echo   %APK_DST%
exit /b 0
