@echo off
setlocal EnableExtensions

for %%I in ("%~dp0\..") do set "ENGINE_ROOT=%%~fI"
set "OGITOR_BIN=%ENGINE_ROOT%\build\run3ogitor-windows-msvc142\RunPath\bin"
set "OGITOR_EXE=%OGITOR_BIN%\qtOgitor.exe"

if not exist "%OGITOR_EXE%" (
    echo ERROR: Ogitor has not been built yet.
    echo Run "%~dp0build.bat" first.
    echo Expected executable: "%OGITOR_EXE%"
    echo.
    pause
    exit /b 1
)

pushd "%OGITOR_BIN%"
"%OGITOR_EXE%" %*
set "RUN_RESULT=%ERRORLEVEL%"
popd

if not "%RUN_RESULT%"=="0" echo Ogitor exited with code %RUN_RESULT%.
exit /b %RUN_RESULT%
