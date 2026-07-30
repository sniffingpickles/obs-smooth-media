function New-SharedUtf8Writer {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $parent = [IO.Path]::GetDirectoryName(
        [IO.Path]::GetFullPath($Path)
    )
    if ($parent) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }

    $sharing = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $stream = [IO.FileStream]::new(
        $Path,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        $sharing
    )
    try {
        $encoding = [Text.UTF8Encoding]::new($false)
        $writer = [IO.StreamWriter]::new(
            $stream,
            $encoding,
            4096,
            $false
        )
        $writer.AutoFlush = $true
        return $writer
    } catch {
        $stream.Dispose()
        throw
    }
}
