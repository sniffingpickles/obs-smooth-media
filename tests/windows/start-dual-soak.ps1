param(
    [Parameter(Mandatory = $true)]
    [string]$RtmpUrl,
    [Parameter(Mandatory = $true)]
    [string]$RistUrl,
    [Parameter(Mandatory = $true)]
    [string]$ResultDirectory,
    [string]$SceneName = "Smooth Media Dual Soak",
    [string]$RtmpSourceName = "Smooth Media RTMP Soak Source",
    [string]$RistSourceName = "Smooth Media RIST Soak Source",
    [ValidateRange(10, 604800)]
    [int]$DurationSeconds = 86400,
    [ValidateRange(1, 3600)]
    [int]$ProcessSampleIntervalSeconds = 60,
    [string]$TaskPrefix = "ObsSmoothDualSoak"
)

$ErrorActionPreference = "Stop"

function Quote-TaskArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    return '"' + $Value.Replace('"', '""') + '"'
}

function Register-SoakTask {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Arguments,
        [Parameter(Mandatory = $true)][string]$Identity
    )

    $existing = Get-ScheduledTask -TaskName $Name `
        -ErrorAction SilentlyContinue
    if ($existing) {
        if ($existing.State -eq "Running") {
            Stop-ScheduledTask -TaskName $Name
        }
        Unregister-ScheduledTask -TaskName $Name -Confirm:$false
    }

    $action = New-ScheduledTaskAction `
        -Execute "powershell.exe" `
        -Argument $Arguments
    $principal = New-ScheduledTaskPrincipal `
        -UserId $Identity `
        -LogonType Interactive `
        -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet `
        -ExecutionTimeLimit (New-TimeSpan -Days 8) `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries
    Register-ScheduledTask `
        -TaskName $Name `
        -Action $action `
        -Principal $principal `
        -Settings $settings `
        -Force | Out-Null
}

if (-not (Get-Process obs64 -ErrorAction SilentlyContinue)) {
    throw "OBS must already be running in the interactive desktop session"
}

$harness = Join-Path $PSScriptRoot "obs-e2e.ps1"
$monitor = Join-Path $PSScriptRoot "monitor-obs-process.ps1"
$sharedWriter = Join-Path $PSScriptRoot "shared-file-writer.ps1"
if (-not (Test-Path $harness) -or -not (Test-Path $monitor) -or
    -not (Test-Path $sharedWriter)) {
    throw "Soak helper scripts are missing beside this file"
}

New-Item -ItemType Directory -Force -Path $ResultDirectory | Out-Null
$resultRoot = (Resolve-Path $ResultDirectory).Path
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$taskNames = [ordered]@{
    rtmp = "$TaskPrefix-RTMP"
    rist = "$TaskPrefix-RIST"
    process = "$TaskPrefix-Process"
}
$resultPaths = [ordered]@{
    rtmp = Join-Path $resultRoot "rtmp-e2e.json"
    rist = Join-Path $resultRoot "rist-e2e.json"
    process = Join-Path $resultRoot "obs-process.csv"
    manifest = Join-Path $resultRoot "manifest.json"
}

$common = "-NoProfile -ExecutionPolicy Bypass -File " +
    (Quote-TaskArgument $harness) +
    " -SceneName " + (Quote-TaskArgument $SceneName) +
    " -StableSeconds $DurationSeconds -RestartIterations 0"
$rtmpArguments = $common +
    " -SourceName " + (Quote-TaskArgument $RtmpSourceName) +
    " -StreamUrl " + (Quote-TaskArgument $RtmpUrl) +
    " -InputFormat flv -ResultPath " +
    (Quote-TaskArgument $resultPaths.rtmp)
$ristArguments = $common +
    " -SourceName " + (Quote-TaskArgument $RistSourceName) +
    " -StreamUrl " + (Quote-TaskArgument $RistUrl) +
    " -InputFormat mpegts -ResultPath " +
    (Quote-TaskArgument $resultPaths.rist)
$monitorDuration = $DurationSeconds + 60
$monitorArguments = "-NoProfile -ExecutionPolicy Bypass -File " +
    (Quote-TaskArgument $monitor) +
    " -ResultPath " + (Quote-TaskArgument $resultPaths.process) +
    " -DurationSeconds $monitorDuration" +
    " -SampleIntervalSeconds $ProcessSampleIntervalSeconds"

$registered = New-Object System.Collections.Generic.List[string]
try {
    Register-SoakTask `
        -Name $taskNames.rtmp `
        -Arguments $rtmpArguments `
        -Identity $identity
    $registered.Add($taskNames.rtmp)
    Register-SoakTask `
        -Name $taskNames.rist `
        -Arguments $ristArguments `
        -Identity $identity
    $registered.Add($taskNames.rist)
    Register-SoakTask `
        -Name $taskNames.process `
        -Arguments $monitorArguments `
        -Identity $identity
    $registered.Add($taskNames.process)

    foreach ($name in $registered) {
        Start-ScheduledTask -TaskName $name
    }
    Start-Sleep -Seconds 3

    $states = foreach ($name in $registered) {
        $task = Get-ScheduledTask -TaskName $name
        $info = Get-ScheduledTaskInfo -TaskName $name
        [PSCustomObject]@{
            name = $name
            state = [string]$task.State
            lastTaskResult = $info.LastTaskResult
        }
    }
    $notRunning = @($states | Where-Object { $_.state -ne "Running" })
    if ($notRunning.Count -gt 0) {
        throw "One or more soak tasks did not remain running: " +
            (($notRunning | ConvertTo-Json -Compress) -join "")
    }

    $manifest = [PSCustomObject]@{
        success = $true
        startedUtc = [DateTime]::UtcNow.ToString("o")
        expectedCompletionUtc = [DateTime]::UtcNow.AddSeconds(
            $monitorDuration
        ).ToString("o")
        durationSeconds = $DurationSeconds
        processMonitorDurationSeconds = $monitorDuration
        processSampleIntervalSeconds = $ProcessSampleIntervalSeconds
        callerSessionId = (Get-Process -Id $PID).SessionId
        obsSessionId = (
            Get-Process obs64 | Select-Object -First 1
        ).SessionId
        identity = $identity
        sceneName = $SceneName
        rtmpSourceName = $RtmpSourceName
        ristSourceName = $RistSourceName
        tasks = $states
        results = $resultPaths
    }
    $manifest | ConvertTo-Json -Depth 10 |
        Set-Content -Encoding UTF8 -Path $resultPaths.manifest
    $manifest | ConvertTo-Json -Depth 10
} catch {
    foreach ($name in $registered) {
        Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
        Unregister-ScheduledTask `
            -TaskName $name `
            -Confirm:$false `
            -ErrorAction SilentlyContinue
    }
    throw
}
