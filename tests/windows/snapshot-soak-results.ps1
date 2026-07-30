param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,
    [Parameter(Mandatory = $true)]
    [string]$SnapshotDirectory
)

$ErrorActionPreference = "Stop"

$sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
$snapshotRoot = [IO.Path]::GetFullPath($SnapshotDirectory)
if ($snapshotRoot -eq $sourceRoot -or
    $snapshotRoot.StartsWith(
        $sourceRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw "SnapshotDirectory must be outside SourceDirectory"
}

[IO.Directory]::CreateDirectory($snapshotRoot) | Out-Null
$sharing = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
$copied = New-Object System.Collections.Generic.List[object]

foreach ($source in Get-ChildItem -LiteralPath $sourceRoot -File) {
    $destination = Join-Path $snapshotRoot $source.Name
    $input = [IO.FileStream]::new(
        $source.FullName,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        $sharing
    )
    try {
        $output = [IO.FileStream]::new(
            $destination,
            [IO.FileMode]::Create,
            [IO.FileAccess]::Write,
            [IO.FileShare]::Read
        )
        try {
            $input.CopyTo($output)
            $output.Flush()
        } finally {
            $output.Dispose()
        }
    } finally {
        $input.Dispose()
    }

    $copy = Get-Item -LiteralPath $destination
    $copied.Add([PSCustomObject]@{
        name = $copy.Name
        length = $copy.Length
        lastWriteTimeUtc = $copy.LastWriteTimeUtc.ToString("o")
    })
}

[PSCustomObject]@{
    success = $true
    sourceDirectory = $sourceRoot
    snapshotDirectory = $snapshotRoot
    files = $copied
    utc = [DateTime]::UtcNow.ToString("o")
} | ConvertTo-Json -Depth 5
