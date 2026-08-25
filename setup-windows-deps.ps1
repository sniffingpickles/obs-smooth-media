# Downloads the Windows runtime and development files used by the release
# build. Versions and hashes are intentionally pinned to the OBS 32.2.2
# dependency manifest.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$DEPS_DIR = Join-Path $PSScriptRoot "deps"

$OBS_VERSION = "32.2.2"
$OBS_SHA256 = "4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1"
$OBS_URL = "https://github.com/obsproject/obs-studio/releases/download/$OBS_VERSION/OBS-Studio-$OBS_VERSION-Windows-x64.zip"
$OBS_ZIP = Join-Path $DEPS_DIR "obs-studio.zip"
$OBS_DIR = Join-Path $DEPS_DIR "obs-studio"
$OBS_SRC = Join-Path $DEPS_DIR "obs-source"

$OBS_DEPS_VERSION = "2026-07-15"
$OBS_DEPS_SHA256 = "6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9"
$OBS_DEPS_URL = "https://github.com/obsproject/obs-deps/releases/download/$OBS_DEPS_VERSION/windows-deps-$OBS_DEPS_VERSION-x64.zip"
$OBS_DEPS_ZIP = Join-Path $DEPS_DIR "obs-deps.zip"
$OBS_DEPS_DIR = Join-Path $DEPS_DIR "obs-deps"

function Assert-Sha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected
    )

    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Expected.ToLowerInvariant()) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for $Path (expected $Expected, received $actual)"
    }
}

function Remove-StaleDependency {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $marker = Join-Path $Path ".smooth-media-version"
    $installedVersion = if (Test-Path -LiteralPath $marker) {
        (Get-Content -LiteralPath $marker -Raw).Trim()
    } else {
        ""
    }

    if ($installedVersion -ne $ExpectedVersion) {
        Write-Host "Refreshing stale dependency at $Path" -ForegroundColor Yellow
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $DEPS_DIR -Force | Out-Null

Remove-StaleDependency -Path $OBS_DIR -ExpectedVersion "obs-$OBS_VERSION"
Remove-StaleDependency -Path $OBS_SRC -ExpectedVersion "obs-source-$OBS_VERSION"
Remove-StaleDependency -Path $OBS_DEPS_DIR -ExpectedVersion "obs-deps-$OBS_DEPS_VERSION"

if (-not (Test-Path -LiteralPath $OBS_DIR)) {
    Write-Host "Downloading OBS Studio $OBS_VERSION..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $OBS_URL -OutFile $OBS_ZIP -UseBasicParsing
    Assert-Sha256 -Path $OBS_ZIP -Expected $OBS_SHA256
    Expand-Archive -LiteralPath $OBS_ZIP -DestinationPath $OBS_DIR -Force
    Remove-Item -LiteralPath $OBS_ZIP -Force

    $subdirectory = Get-ChildItem -LiteralPath $OBS_DIR -Directory |
        Select-Object -First 1
    if ($subdirectory -and $subdirectory.Name -ne "bin") {
        Get-ChildItem -LiteralPath $subdirectory.FullName |
            Move-Item -Destination $OBS_DIR -Force
        Remove-Item -LiteralPath $subdirectory.FullName -Recurse -Force
    }

    New-Item -ItemType File -Path (Join-Path $OBS_DIR "portable_mode.txt") -Force |
        Out-Null
    Set-Content -LiteralPath (Join-Path $OBS_DIR ".smooth-media-version") `
        -Value "obs-$OBS_VERSION" -NoNewline
}

if (-not (Test-Path -LiteralPath $OBS_DEPS_DIR)) {
    Write-Host "Downloading OBS build dependencies $OBS_DEPS_VERSION..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $OBS_DEPS_URL -OutFile $OBS_DEPS_ZIP -UseBasicParsing
    Assert-Sha256 -Path $OBS_DEPS_ZIP -Expected $OBS_DEPS_SHA256
    Expand-Archive -LiteralPath $OBS_DEPS_ZIP -DestinationPath $OBS_DEPS_DIR -Force
    Remove-Item -LiteralPath $OBS_DEPS_ZIP -Force
    Set-Content -LiteralPath (Join-Path $OBS_DEPS_DIR ".smooth-media-version") `
        -Value "obs-deps-$OBS_DEPS_VERSION" -NoNewline
}

if (-not (Test-Path -LiteralPath $OBS_SRC)) {
    Write-Host "Cloning OBS Studio $OBS_VERSION source headers..." -ForegroundColor Cyan
    git clone --depth 1 --branch $OBS_VERSION --filter=blob:none --sparse `
        "https://github.com/obsproject/obs-studio.git" $OBS_SRC
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to clone OBS Studio source"
    }

    Push-Location $OBS_SRC
    try {
        git sparse-checkout set libobs deps/w32-pthreads
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to configure the OBS sparse checkout"
        }
    } finally {
        Pop-Location
    }
    Set-Content -LiteralPath (Join-Path $OBS_SRC ".smooth-media-version") `
        -Value "obs-source-$OBS_VERSION" -NoNewline
}

Write-Host "Dependencies are ready." -ForegroundColor Green
Write-Host "Build with:"
Write-Host "  .\build-windows.bat `"$OBS_DIR`" `"$OBS_DEPS_DIR`""
Write-Host ""
Write-Host "Direct CMake configuration:"
Write-Host "  cmake -B build -G `"Visual Studio 17 2022`" -A x64 ``"
Write-Host "    -DOBS_DIR=`"$OBS_DIR`" ``"
Write-Host "    -DOBS_INCLUDE_DIR=`"$OBS_SRC/libobs`" ``"
Write-Host "    -DFFMPEG_DIR=`"$OBS_DEPS_DIR`""
