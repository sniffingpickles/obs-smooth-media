param(
    [Parameter(Mandatory = $true)]
    [string]$ObsDirectory,

    [string]$OutputDirectory = (Join-Path $ObsDirectory "lib")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

foreach ($tool in @("dumpbin.exe", "lib.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found. Run this script from a Visual Studio developer environment."
    }
}

function New-ImportLibrary {
    param(
        [Parameter(Mandatory = $true)][string]$DllPath,
        [Parameter(Mandatory = $true)][string]$LibraryName
    )

    $exports = & dumpbin.exe /nologo /exports $DllPath
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $DllPath"
    }

    $symbols = foreach ($line in $exports) {
        if ($line -match "^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)") {
            $Matches[1]
        }
    }
    if (-not $symbols) {
        throw "No exports were found in $DllPath"
    }

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $definitionPath = Join-Path $OutputDirectory "$LibraryName.def"
    $libraryPath = Join-Path $OutputDirectory "$LibraryName.lib"
    $definition = @("LIBRARY $LibraryName", "EXPORTS") +
        ($symbols | ForEach-Object { "  $_" })
    Set-Content -LiteralPath $definitionPath -Value $definition -Encoding Ascii

    & lib.exe /nologo /def:$definitionPath /out:$libraryPath /machine:x64
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $libraryPath)) {
        throw "Failed to generate $libraryPath"
    }

    Write-Host "Generated $libraryPath"
}

$obsDll = Get-ChildItem -LiteralPath $ObsDirectory -Recurse -Filter "obs.dll" |
    Select-Object -First 1
if (-not $obsDll) {
    throw "obs.dll was not found under $ObsDirectory"
}
New-ImportLibrary -DllPath $obsDll.FullName -LibraryName "obs"

$pthreadsDll = Get-ChildItem -LiteralPath $ObsDirectory -Recurse `
    -Filter "w32-pthreads.dll" | Select-Object -First 1
if (-not $pthreadsDll) {
    throw "w32-pthreads.dll was not found under $ObsDirectory"
}
New-ImportLibrary -DllPath $pthreadsDll.FullName -LibraryName "w32-pthreads"
