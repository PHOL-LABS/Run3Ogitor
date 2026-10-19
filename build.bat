@echo off
setlocal EnableExtensions

call :build_ogitor
set "BUILD_RESULT=%ERRORLEVEL%"

echo.
if "%BUILD_RESULT%"=="0" (
    echo Ogitor Release build completed successfully.
    echo Executable: %~dp0..\build\run3ogitor-windows-msvc142\RunPath\bin\qtOgitor.exe
) else (
    echo Ogitor build failed with exit code %BUILD_RESULT%.
)
echo.
pause
exit /b %BUILD_RESULT%

:build_ogitor
for %%I in ("%~dp0.") do set "OGITOR_ROOT=%%~fI"
for %%I in ("%OGITOR_ROOT%\..") do set "ENGINE_ROOT=%%~fI"

set "QT_ROOT=C:\Qt\5.15.2\msvc2019_64"
set "OGRE_SDK=%ENGINE_ROOT%\OgreSDK"
set "BUILD_ROOT=%ENGINE_ROOT%\build"
set "BUILD_DIR=%BUILD_ROOT%\run3ogitor-windows-msvc142"
set "OGRE_SOURCE=%BUILD_ROOT%\ogre-14.6.0-source"
set "BRIDGE_BUILD=%BUILD_ROOT%\run3ogitor-ogre-bitesqt5"
set "BRIDGE_SOURCE=%OGITOR_ROOT%\build-support\ogre-bites-qt5"
set "OGRE_QT5_CONFIG=%OGITOR_ROOT%\build-support\ogre-qt5-config"
set "DEPLOY_BIN=%BUILD_DIR%\RunPath\bin"

if not exist "%QT_ROOT%\bin\qmake.exe" (
    echo ERROR: Qt 5.15.2 MSVC installation was not found at "%QT_ROOT%".
    exit /b 1
)
if not exist "%OGRE_SDK%\lib\OgreMain.lib" (
    echo ERROR: Ogre SDK was not found at "%OGRE_SDK%".
    exit /b 1
)
where cmake.exe >nul 2>nul || (
    echo ERROR: cmake.exe is not available in PATH.
    exit /b 1
)
where ninja.exe >nul 2>nul || (
    echo ERROR: ninja.exe is not available in PATH.
    exit /b 1
)
where git.exe >nul 2>nul || (
    echo ERROR: git.exe is not available in PATH.
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio Installer's vswhere.exe was not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT (
    echo ERROR: No Visual Studio C++ installation was found.
    exit /b 1
)
if not exist "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat was not found below "%VS_ROOT%".
    exit /b 1
)

echo Initializing MSVC 14.29 for Qt 5.15 compatibility...
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.29
if errorlevel 1 (
    echo ERROR: MSVC toolset 14.29 is required but could not be initialized.
    exit /b 1
)

echo Initializing Ogitor submodules...
git -C "%OGITOR_ROOT%" submodule update --init --recursive
if errorlevel 1 exit /b 1

if not exist "%OGRE_SOURCE%\CMakeLists.txt" (
    echo Downloading the matching official Ogre 14.6.0 source...
    git clone --depth 1 --branch v14.6.0 https://github.com/OGRECave/ogre.git "%OGRE_SOURCE%"
    if errorlevel 1 exit /b 1
)

echo Building the Ogre Qt 5 bridge...
cmake -S "%BRIDGE_SOURCE%" -B "%BRIDGE_BUILD%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_PREFIX_PATH=%QT_ROOT%" ^
    "-DOGRE_SDK=%OGRE_SDK%" ^
    "-DOGRE_SOURCE=%OGRE_SOURCE%"
if errorlevel 1 exit /b 1
cmake --build "%BRIDGE_BUILD%" --parallel
if errorlevel 1 exit /b 1

set "RUN3OGITOR_OGRE_SDK=%OGRE_SDK%"
set "RUN3OGITOR_QT5_BRIDGE_DIR=%BRIDGE_BUILD%"

echo Configuring Ogitor...
cmake -S "%OGITOR_ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_PREFIX_PATH=%QT_ROOT%;%OGRE_SDK%" ^
    "-DOGRE_DIR=%OGRE_QT5_CONFIG%" ^
    -DOGITOR_DOWNLOAD_SAMPLEPROJECT=ON ^
    -DOGITOR_DIST=OFF ^
    "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /D_HAS_AUTO_PTR_ETC=1"
if errorlevel 1 exit /b 1

echo Building Ogitor...
cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 exit /b 1

echo Installing Ogitor...
echo Checking native Run3 document serialization...
set "PATH=%QT_ROOT%\bin;%PATH%"
"%BUILD_DIR%\Plugins\Run3Serializer\run3_document_tests.exe"
if errorlevel 1 exit /b 1

cmake --install "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

echo Deploying Ogre and Qt runtime files...
for %%F in ("%OGRE_SDK%\bin\*.dll") do (
    if /I not "%%~nxF"=="OgreBitesQt.dll" copy /Y "%%~fF" "%DEPLOY_BIN%\" >nul
)
copy /Y "%BRIDGE_BUILD%\bin\OgreBitesQt.dll" "%DEPLOY_BIN%\" >nul
if errorlevel 1 exit /b 1
copy /Y "%OGRE_SDK%\bin\plugins.cfg" "%DEPLOY_BIN%\" >nul
if errorlevel 1 exit /b 1
"%QT_ROOT%\bin\windeployqt.exe" --release --compiler-runtime --no-translations "%DEPLOY_BIN%\qtOgitor.exe"
if errorlevel 1 exit /b 1

exit /b 0
