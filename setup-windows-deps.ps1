# ============================================================
#  Download OBS Portable + FFmpeg dev libs for Windows builds
#
#  Run this in PowerShell on Windows:
#    .\setup-windows-deps.ps1
#
#  This will create:
#    deps/obs-studio/   - Portable OBS with headers
#    deps/ffmpeg/       - FFmpeg dev libraries
# ============================================================

$ErrorActionPreference = "Stop"

$DEPS_DIR = Join-Path $PSScriptRoot "deps"
$OBS_VERSION = "32.0.4"
$FFMPEG_VERSION = "7.1"

# OBS Portable download
$OBS_URL = "https://github.com/obsproject/obs-studio/releases/download/$OBS_VERSION/OBS-Studio-$OBS_VERSION-Windows-x64.zip"
$OBS_ZIP = Join-Path $DEPS_DIR "obs-studio.zip"
$OBS_DIR = Join-Path $DEPS_DIR "obs-studio"

# FFmpeg shared dev download (from gyan.dev)
$FFMPEG_URL = "https://github.com/GyanD/codexffmpeg/releases/download/$FFMPEG_VERSION/ffmpeg-$FFMPEG_VERSION-full_build-shared.zip"
$FFMPEG_ZIP = Join-Path $DEPS_DIR "ffmpeg.zip"
$FFMPEG_DIR = Join-Path $DEPS_DIR "ffmpeg"

if (-not (Test-Path $DEPS_DIR)) {
    New-Item -ItemType Directory -Path $DEPS_DIR | Out-Null
}

# Download and extract OBS
if (-not (Test-Path $OBS_DIR)) {
    Write-Host "Downloading OBS Studio $OBS_VERSION portable..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $OBS_URL -OutFile $OBS_ZIP -UseBasicParsing
    Write-Host "Extracting OBS..."
    Expand-Archive -Path $OBS_ZIP -DestinationPath $OBS_DIR -Force
    Remove-Item $OBS_ZIP

    # OBS portable zip extracts into a subdirectory — flatten if needed
    $sub = Get-ChildItem $OBS_DIR -Directory | Select-Object -First 1
    if ($sub -and $sub.Name -ne "bin") {
        Get-ChildItem $sub.FullName | Move-Item -Destination $OBS_DIR -Force
        Remove-Item $sub.FullName -Recurse -Force
    }

    # Create portable_mode.txt so OBS runs in portable mode
    New-Item -ItemType File -Path (Join-Path $OBS_DIR "portable_mode.txt") -Force | Out-Null

    Write-Host "OBS Studio extracted to: $OBS_DIR" -ForegroundColor Green
} else {
    Write-Host "OBS Studio already exists at: $OBS_DIR" -ForegroundColor Yellow
}

# Download and extract FFmpeg dev libs
if (-not (Test-Path $FFMPEG_DIR)) {
    Write-Host "Downloading FFmpeg $FFMPEG_VERSION dev libraries..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $FFMPEG_URL -OutFile $FFMPEG_ZIP -UseBasicParsing
    Write-Host "Extracting FFmpeg..."
    Expand-Archive -Path $FFMPEG_ZIP -DestinationPath $FFMPEG_DIR -Force
    Remove-Item $FFMPEG_ZIP

    # Flatten if extracted into subdirectory
    $sub = Get-ChildItem $FFMPEG_DIR -Directory | Select-Object -First 1
    if ($sub -and $sub.Name -like "ffmpeg-*") {
        Get-ChildItem $sub.FullName | Move-Item -Destination $FFMPEG_DIR -Force
        Remove-Item $sub.FullName -Recurse -Force
    }

    Write-Host "FFmpeg extracted to: $FFMPEG_DIR" -ForegroundColor Green
} else {
    Write-Host "FFmpeg already exists at: $FFMPEG_DIR" -ForegroundColor Yellow
}

# We also need OBS headers from the source repo
$OBS_SRC = Join-Path $DEPS_DIR "obs-source"
if (-not (Test-Path $OBS_SRC)) {
    Write-Host "Cloning OBS Studio source (headers only)..." -ForegroundColor Cyan
    git clone --depth 1 --branch $OBS_VERSION --filter=blob:none --sparse "https://github.com/obsproject/obs-studio.git" $OBS_SRC
    Push-Location $OBS_SRC
    git sparse-checkout set libobs deps/w32-pthreads
    Pop-Location
    Write-Host "OBS source headers at: $OBS_SRC" -ForegroundColor Green
} else {
    Write-Host "OBS source already exists at: $OBS_SRC" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Dependencies ready!" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Build with:" -ForegroundColor White
Write-Host "  .\build-windows.bat `"$OBS_DIR`" `"$FFMPEG_DIR`"" -ForegroundColor White
Write-Host ""
Write-Host "Or with CMake directly:" -ForegroundColor White
Write-Host "  cmake -B build -G `"Visual Studio 17 2022`" -A x64 ``" -ForegroundColor White
Write-Host "    -DOBS_DIR=`"$OBS_DIR`" ``" -ForegroundColor White
Write-Host "    -DOBS_INCLUDE_DIR=`"$OBS_SRC/libobs`" ``" -ForegroundColor White
Write-Host "    -DFFMPEG_DIR=`"$FFMPEG_DIR`"" -ForegroundColor White
Write-Host ""
