@echo off
setlocal

set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "OBS_DIR=%~f1"
set "OBS_DEPS_DIR=%~f2"

if "%~1"=="" goto :usage
if "%~2"=="" goto :usage

if "%~3"=="" (
    for %%I in ("%OBS_DIR%\..\obs-source") do set "OBS_SOURCE_DIR=%%~fI"
) else (
    set "OBS_SOURCE_DIR=%~f3"
)

if not exist "%OBS_DIR%\bin" (
    echo OBS runtime not found at "%OBS_DIR%".
    exit /b 1
)
if not exist "%OBS_SOURCE_DIR%\libobs\obs-module.h" (
    echo OBS source headers not found at "%OBS_SOURCE_DIR%\libobs".
    exit /b 1
)
if not exist "%OBS_DEPS_DIR%\include\libavformat\avformat.h" (
    echo OBS build dependencies not found at "%OBS_DEPS_DIR%".
    exit /b 1
)

where dumpbin.exe >nul 2>nul
if errorlevel 1 call :setup_msvc
if errorlevel 1 exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%PROJECT_DIR%\scripts\generate-windows-import-libs.ps1" ^
    -ObsDirectory "%OBS_DIR%"
if errorlevel 1 exit /b 1

cmake -S "%PROJECT_DIR%" -B "%PROJECT_DIR%\build_win" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DOBS_DIR="%OBS_DIR%" ^
    -DOBS_INCLUDE_DIR="%OBS_SOURCE_DIR%\libobs" ^
    -DOBS_LIB="%OBS_DIR%\lib\obs.lib" ^
    -DW32_PTHREADS_INCLUDE_DIR="%OBS_SOURCE_DIR%\deps\w32-pthreads" ^
    -DW32_PTHREADS_LIB="%OBS_DIR%\lib\w32-pthreads.lib" ^
    -DFFMPEG_DIR="%OBS_DEPS_DIR%" ^
    -DBUILD_TESTING=OFF
if errorlevel 1 exit /b 1

cmake --build "%PROJECT_DIR%\build_win" --config Release
if errorlevel 1 exit /b 1

echo Built: %PROJECT_DIR%\build_win\Release\obs-smooth-media.dll
exit /b 0

:setup_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio locator not found at "%VSWHERE%".
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%I"
if not defined VS_DIR (
    echo Visual Studio 2022 with the C++ desktop workload was not found.
    exit /b 1
)
call "%VS_DIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
exit /b %errorlevel%

:usage
echo Usage: build-windows.bat OBS_DIR OBS_DEPS_DIR [OBS_SOURCE_DIR]
echo.
echo Run setup-windows-deps.ps1 first, then use:
echo   build-windows.bat "deps\obs-studio" "deps\obs-deps"
exit /b 1
