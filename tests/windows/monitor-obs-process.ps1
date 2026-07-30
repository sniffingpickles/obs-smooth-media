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
. (Join-Path $PSScriptRoot "shared-file-writer.ps1")

function Write-ProcessSample {
    param([Parameter(Mandatory = $true)]$Sample)

    $csv = @($Sample | ConvertTo-Csv -NoTypeInformation)
    if (-not $script:CsvHeaderWritten) {
        $script:ResultWriter.WriteLine($csv[0])
        $script:CsvHeaderWritten = $true
    }
    $script:ResultWriter.WriteLine($csv[-1])
}

$script:ResultWriter = New-SharedUtf8Writer -Path $ResultPath
$script:CsvHeaderWritten = $false
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
    $samples = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        $process = Get-Process obs64 -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $process) {
            Write-ProcessSample ([PSCustomObject]@{
                Utc = [DateTime]::UtcNow.ToString("o")
                Alive = $false
                ProcessId = $null
                WorkingSet = $null
                Private = $null
                Handles = $null
                Threads = $null
                CpuSeconds = $null
            })
            throw "obs64 exited during process monitoring"
        }

        Write-ProcessSample ([PSCustomObject]@{
            Utc = [DateTime]::UtcNow.ToString("o")
            Alive = $true
            ProcessId = $process.Id
            WorkingSet = [long]$process.WorkingSet64
            Private = [long]$process.PrivateMemorySize64
            Handles = $process.HandleCount
            Threads = $process.Threads.Count
            CpuSeconds = [double]$process.CPU
        })
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
} finally {
    $script:ResultWriter.Dispose()
    $script:ResultWriter = $null
}
