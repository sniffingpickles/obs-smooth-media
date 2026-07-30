param(
    [int]$ProcessId,
    [ValidateRange(1, 300)]
    [int]$TimeoutSeconds = 30,
    [string]$ResultPath,
    [string]$TaskName
)

$ErrorActionPreference = "Stop"

if (-not $ProcessId) {
    $process = Get-Process obs64 -ErrorAction Stop |
        Select-Object -First 1
    $ProcessId = $process.Id
} else {
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
}

$closeScript = Join-Path $PSScriptRoot "close-obs.ps1"
if (-not (Test-Path $closeScript)) {
    throw "Close helper not found: $closeScript"
}

if (-not $TaskName) {
    $TaskName = "ObsSmoothShutdownAudit-$PID"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$escapedScript = $closeScript.Replace('"', '""')
$arguments = "-NoProfile -ExecutionPolicy Bypass -File " +
    "`"$escapedScript`" -ProcessId $ProcessId"
$action = New-ScheduledTaskAction `
    -Execute "powershell.exe" `
    -Argument $arguments
$principal = New-ScheduledTaskPrincipal `
    -UserId $identity `
    -LogonType Interactive `
    -RunLevel Highest

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$exited = $false
$taskResult = $null
try {
    Register-ScheduledTask `
        -TaskName $TaskName `
        -Action $action `
        -Principal $principal `
        -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    $exited = $process.WaitForExit($TimeoutSeconds * 1000)

    $deadline = [DateTime]::UtcNow.AddSeconds(2)
    do {
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        $taskResult = $info.LastTaskResult
        if ($taskResult -ne 267009) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
} finally {
    $stopwatch.Stop()
    Unregister-ScheduledTask `
        -TaskName $TaskName `
        -Confirm:$false `
        -ErrorAction SilentlyContinue
}

$result = [PSCustomObject]@{
    success = $exited
    processId = $ProcessId
    elapsedMs = $stopwatch.ElapsedMilliseconds
    timeoutSeconds = $TimeoutSeconds
    taskLastResult = $taskResult
    callerSessionId = (Get-Process -Id $PID).SessionId
    obsSessionId = $process.SessionId
    utc = [DateTime]::UtcNow.ToString("o")
}

if ($ResultPath) {
    $result | ConvertTo-Json |
        Set-Content -Encoding UTF8 -Path $ResultPath
}
$result | ConvertTo-Json

if (-not $exited) {
    throw "OBS did not exit within $TimeoutSeconds seconds"
}
