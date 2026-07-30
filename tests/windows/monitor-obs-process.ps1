param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 604800)]
    [int]$DurationSeconds,
    [ValidateRange(1, 3600)]
    [int]$SampleIntervalSeconds = 5
)

$ErrorActionPreference = "Stop"
Remove-Item $ResultPath -ErrorAction SilentlyContinue

$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
$samples = 0
while ([DateTime]::UtcNow -lt $deadline) {
    $process = Get-Process obs64 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $process) {
        [PSCustomObject]@{
            Utc = [DateTime]::UtcNow.ToString("o")
            Alive = $false
            ProcessId = $null
            WorkingSet = $null
            Private = $null
            Handles = $null
            Threads = $null
            CpuSeconds = $null
        } | Export-Csv -NoTypeInformation -Append -Path $ResultPath
        throw "obs64 exited during process monitoring"
    }

    [PSCustomObject]@{
        Utc = [DateTime]::UtcNow.ToString("o")
        Alive = $true
        ProcessId = $process.Id
        WorkingSet = [long]$process.WorkingSet64
        Private = [long]$process.PrivateMemorySize64
        Handles = $process.HandleCount
        Threads = $process.Threads.Count
        CpuSeconds = [double]$process.CPU
    } | Export-Csv -NoTypeInformation -Append -Path $ResultPath
    $samples++
    Start-Sleep -Seconds $SampleIntervalSeconds
}

[PSCustomObject]@{
    success = $true
    resultPath = $ResultPath
    durationSeconds = $DurationSeconds
    sampleIntervalSeconds = $SampleIntervalSeconds
    samples = $samples
    utc = [DateTime]::UtcNow.ToString("o")
} | ConvertTo-Json
