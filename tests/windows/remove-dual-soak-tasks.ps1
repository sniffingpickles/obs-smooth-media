param(
    [string]$TaskPrefix = "ObsSmoothDualSoak"
)

$ErrorActionPreference = "Stop"
$names = @(
    "$TaskPrefix-RTMP",
    "$TaskPrefix-RIST",
    "$TaskPrefix-Process"
)
$removed = New-Object System.Collections.Generic.List[string]
foreach ($name in $names) {
    $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    if (-not $task) {
        continue
    }
    if ($task.State -eq "Running") {
        Stop-ScheduledTask -TaskName $name
    }
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
    $removed.Add($name)
}

[PSCustomObject]@{
    success = $true
    removed = $removed
    utc = [DateTime]::UtcNow.ToString("o")
} | ConvertTo-Json -Depth 5
