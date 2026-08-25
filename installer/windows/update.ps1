param(
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Repository = "sniffingpickles/obs-smooth-media"
$AssetName = "obs-smooth-media-windows-x64-setup.exe"
$RegistryPath = "HKLM:\Software\Smooth Media Source"

Add-Type -AssemblyName System.Windows.Forms

function Show-Message {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [System.Windows.Forms.MessageBoxIcon]$Icon =
            [System.Windows.Forms.MessageBoxIcon]::Information
    )

    [System.Windows.Forms.MessageBox]::Show(
        $Message,
        "Smooth Media Source Updater",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        $Icon
    ) | Out-Null
}

try {
    $installedVersion = "0.0.0"
    if (Test-Path -LiteralPath $RegistryPath) {
        $value = Get-ItemProperty -LiteralPath $RegistryPath -Name Version `
            -ErrorAction SilentlyContinue
        if ($value -and $value.Version) {
            $installedVersion = [string]$value.Version
        }
    }

    $headers = @{
        Accept = "application/vnd.github+json"
        "X-GitHub-Api-Version" = "2022-11-28"
        "User-Agent" = "obs-smooth-media-updater"
    }
    $release = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/$Repository/releases/latest" `
        -Headers $headers
    $releaseVersion = ([string]$release.tag_name) -replace '^v', ''

    if ([version]$releaseVersion -le [version]$installedVersion) {
        if ($CheckOnly) {
            Write-Output "Installed version $installedVersion is current."
            exit 0
        }
        Show-Message "Smooth Media Source $installedVersion is up to date."
        exit 0
    }

    $asset = @($release.assets) |
        Where-Object { $_.name -eq $AssetName } |
        Select-Object -First 1
    if (-not $asset) {
        throw "The latest release does not contain the Windows installer."
    }
    if ([string]$asset.digest -notmatch '^sha256:([0-9a-fA-F]{64})$') {
        throw "GitHub did not provide a valid installer digest."
    }
    $expectedHash = $Matches[1].ToLowerInvariant()

    if ($CheckOnly) {
        Write-Output "Update $releaseVersion is available with a valid digest."
        exit 0
    }

    $choice = [System.Windows.Forms.MessageBox]::Show(
        "Smooth Media Source $releaseVersion is available. Install it now?",
        "Smooth Media Source Updater",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Question
    )
    if ($choice -ne [System.Windows.Forms.DialogResult]::Yes) {
        exit 0
    }

    $downloadPath = Join-Path $env:TEMP `
        "obs-smooth-media-$releaseVersion-windows-x64-setup.exe"
    Remove-Item -LiteralPath $downloadPath -Force -ErrorAction SilentlyContinue
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $downloadPath `
        -UseBasicParsing

    $downloadHash = Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256
    $actualHash = $downloadHash.Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        Remove-Item -LiteralPath $downloadPath -Force -ErrorAction SilentlyContinue
        throw "The downloaded installer failed SHA-256 verification."
    }

    Start-Process -FilePath $downloadPath -Verb RunAs
} catch {
    Show-Message $_.Exception.Message `
        ([System.Windows.Forms.MessageBoxIcon]::Error)
    exit 1
}
