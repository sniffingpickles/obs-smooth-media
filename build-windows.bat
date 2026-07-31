@echo off
REM ============================================================
REM  Build script for obs-smooth-media plugin (Windows)
REM
REM  Prerequisites:
REM    1. Visual Studio 2022 (with C++ Desktop Development)
REM    2. CMake 3.16+ (in PATH)
REM    3. OBS Studio installed or portable
REM    4. FFmpeg dev libs (headers + import libs)
REM
REM  Usage:
REM    build-windows.bat [OBS_DIR] [FFMPEG_DIR]
REM
REM  Example:
REM    build-windows.bat "C:\obs-studio" "C:\ffmpeg"
REM ============================================================

setlocal

set OBS_DIR=%~1
set FFMPEG_DIR=%~2

if "%OBS_DIR%"=="" (
    echo Usage: build-windows.bat [OBS_DIR] [FFMPEG_DIR]
    echo.
    echo   OBS_DIR    = Path to OBS Studio install (contains bin/, include/, lib/)
    echo   FFMPEG_DIR = Path to FFmpeg dev package (contains include/, lib/)
    echo.
    echo   If OBS includes FFmpeg headers/libs, you can set FFMPEG_DIR = OBS_DIR.
    exit /b 1
)

if "%FFMPEG_DIR%"=="" set FFMPEG_DIR=%OBS_DIR%

echo.
echo === Building obs-smooth-media ===
echo OBS_DIR:    %OBS_DIR%
echo FFMPEG_DIR: %FFMPEG_DIR%
echo.

if not exist build_win mkdir build_win
cd build_win

cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DOBS_DIR="%OBS_DIR%" ^
    -DFFMPEG_DIR="%FFMPEG_DIR%" ^
    -DBUILD_TESTING=OFF

if %ERRORLEVEL% neq 0 (
    echo.
    echo CMake configuration failed!
    exit /b 1
)

cmake --build . --config Release

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed!
    exit /b 1
)

echo.
echo === Build successful! ===
echo.
echo Plugin DLL: build_win\Release\obs-smooth-media.dll
echo.
echo To install, copy the DLL to your OBS plugins directory:
echo   %OBS_DIR%\obs-plugins\64bit\obs-smooth-media.dll
echo.
echo And copy locale data:
echo   xcopy /s /i ..\data %OBS_DIR%\data\obs-plugins\obs-smooth-media
echo.

cd ..
endlocal
