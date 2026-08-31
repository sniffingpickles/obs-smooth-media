param(
    [string]$WebSocketUri = "ws://127.0.0.1:4455",
    [string]$SceneName = "Smooth Media Audit",
    [string]$SourceName = "Smooth Media Audit Source",
    [Parameter(Mandatory = $true)]
    [string]$StreamUrl,
    [string]$InputFormat = "mpegts",
    [int]$StableSeconds = 30,
    [int]$RestartIterations = 20,
    [switch]$HardwareDecoding,
    [switch]$StrictPtsSync,
    [switch]$ExpectReconnectCycle,
    [switch]$AllowTransientDisconnects,
    [switch]$SkipConnectionMutationChecks,
    [switch]$RecordDuringTest,
    [switch]$CleanupOnExit,
    [int]$RequestTimeoutSeconds = 10,
    [ValidateRange(1, 60)]
    [int]$FinalLivenessSeconds = 3,
    [string]$FinalStreamUrl,
    [string]$ResultPath
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "shared-file-writer.ps1")

$script:Socket = $null
$script:Identified = $false
$script:Observations = New-Object System.Collections.Generic.List[object]
$script:CheckpointWriter = $null
$script:RecordingStarted = $false
$script:RecordingOutputPath = $null
$script:CheckpointPath = if ($ResultPath) {
    if ([IO.Path]::GetExtension($ResultPath) -eq ".json") {
        [IO.Path]::ChangeExtension($ResultPath, ".jsonl")
    } else {
        "$ResultPath.jsonl"
    }
} else {
    $null
}
if ($RequestTimeoutSeconds -lt 1) {
    throw "RequestTimeoutSeconds must be at least 1"
}
if ($script:CheckpointPath) {
    $script:CheckpointWriter = New-SharedUtf8Writer `
        -Path $script:CheckpointPath
}

function Send-WebSocketJson {
    param([Parameter(Mandatory = $true)]$Value)

    $json = $Value | ConvertTo-Json -Depth 30 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    $null = $script:Socket.SendAsync(
        $segment,
        [Net.WebSockets.WebSocketMessageType]::Text,
        $true,
        [Threading.CancellationToken]::None
    ).GetAwaiter().GetResult()
}

function Receive-WebSocketJson {
    $buffer = New-Object byte[] 65536
    $stream = New-Object IO.MemoryStream
    $cancellation = New-Object Threading.CancellationTokenSource
    $cancellation.CancelAfter(
        [TimeSpan]::FromSeconds($RequestTimeoutSeconds)
    )
    try {
        do {
            $segment = [ArraySegment[byte]]::new($buffer)
            $result = $script:Socket.ReceiveAsync(
                $segment,
                $cancellation.Token
            ).GetAwaiter().GetResult()
            if ($result.MessageType -eq
                [Net.WebSockets.WebSocketMessageType]::Close) {
                throw "OBS WebSocket closed unexpectedly"
            }
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)

        $text = [Text.Encoding]::UTF8.GetString($stream.ToArray())
        return $text | ConvertFrom-Json
    } catch [OperationCanceledException] {
        throw "Timed out after $RequestTimeoutSeconds seconds waiting for OBS WebSocket"
    } finally {
        $cancellation.Dispose()
        $stream.Dispose()
    }
}

function Invoke-ObsRequest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RequestType,
        $RequestData = @{},
        [switch]$AllowFailure
    )

    $requestId = [Guid]::NewGuid().ToString("N")
    Send-WebSocketJson @{
        op = 6
        d = @{
            requestType = $RequestType
            requestId = $requestId
            requestData = $RequestData
        }
    }

    do {
        $message = Receive-WebSocketJson
    } while ($message.op -ne 7 -or
             $message.d.requestId -ne $requestId)

    if (-not $message.d.requestStatus.result -and -not $AllowFailure) {
        $status = $message.d.requestStatus
        throw "$RequestType failed ($($status.code)): $($status.comment)"
    }
    return $message.d
}

function Invoke-VendorRequest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RequestType,
        $RequestData = @{}
    )

    $result = Invoke-ObsRequest "CallVendorRequest" @{
        vendorName = "obs-smooth-media"
        requestType = $RequestType
        requestData = $RequestData
    }
    return $result.responseData.responseData
}

function Assert-Condition {
    param(
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Get-SmoothStatus {
    $status = Invoke-VendorRequest "GetStatus" @{
        sourceName = $SourceName
    }
    Assert-Condition $status.success "GetStatus returned failure"
    $observation = [PSCustomObject]@{
        utc = [DateTime]::UtcNow.ToString("o")
        state = $status.state
        active = [bool]$status.active
        reconnecting = [bool]$status.reconnecting
        audioFramesOut = [long]$status.audioFramesOut
        videoFramesOut = [long]$status.videoFramesOut
        avOffsetMs = [long]$status.avOffsetMs
    }
    $script:Observations.Add($observation)
    if ($script:CheckpointWriter) {
        $script:CheckpointWriter.WriteLine(
            ($observation | ConvertTo-Json -Compress)
        )
    }
    return $status
}

function Wait-ForPlayback {
    param(
        [int]$TimeoutSeconds = 20,
        [long]$AfterVideoFrames = 0
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $status = Get-SmoothStatus
        if ($status.state -eq "playing" -and $status.active -and
            [long]$status.videoFramesOut -gt $AfterVideoFrames) {
            return $status
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Playback did not recover within $TimeoutSeconds seconds"
}

function Wait-ForRestartRecovery {
    param(
        [Parameter(Mandatory = $true)]
        [long]$BeforeVideoFrames,
        [int]$TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $restartObserved = $false
    do {
        $status = Get-SmoothStatus
        if ([long]$status.videoFramesOut -lt $BeforeVideoFrames -or
            $status.state -ne "playing" -or -not $status.active) {
            $restartObserved = $true
        }
        if ($restartObserved -and $status.state -eq "playing" -and
            $status.active -and [long]$status.videoFramesOut -gt 5) {
            return $status
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Restart was not observed and recovered within $TimeoutSeconds seconds"
}

try {
    $script:Socket = New-Object Net.WebSockets.ClientWebSocket
    $null = $script:Socket.ConnectAsync(
        [Uri]$WebSocketUri,
        [Threading.CancellationToken]::None
    ).GetAwaiter().GetResult()

    $hello = Receive-WebSocketJson
    Assert-Condition ($hello.op -eq 0) "Expected OBS WebSocket Hello"
    Send-WebSocketJson @{
        op = 1
        d = @{
            rpcVersion = 1
            eventSubscriptions = 0
        }
    }
    $identified = Receive-WebSocketJson
    Assert-Condition ($identified.op -eq 2) "OBS WebSocket identification failed"
    $script:Identified = $true

    $sceneCreate = Invoke-ObsRequest "CreateScene" @{
        sceneName = $SceneName
    } -AllowFailure
    if (-not $sceneCreate.requestStatus.result -and
        $sceneCreate.requestStatus.code -ne 601) {
        throw "Unable to create test scene: $($sceneCreate.requestStatus.comment)"
    }

    Invoke-ObsRequest "SetCurrentProgramScene" @{
        sceneName = $SceneName
    } | Out-Null

    $inputSettings = @{
        input = $StreamUrl
        input_format = $InputFormat
        reconnect_delay_sec = 1
        hw_decode = [bool]$HardwareDecoding
        sync_pts = [bool]$StrictPtsSync
        adaptive_audio_speed = $false
        close_when_inactive = $false
        disable_video = $false
        debug_logging = $true
    }
    $inputList = Invoke-ObsRequest "GetInputList"
    $existing = @(
        $inputList.responseData.inputs |
            Where-Object inputName -eq $SourceName
    )
    if ($existing.Count -eq 0) {
        Invoke-ObsRequest "CreateInput" @{
            sceneName = $SceneName
            inputName = $SourceName
            inputKind = "smooth_media_source"
            inputSettings = $inputSettings
            sceneItemEnabled = $true
        } | Out-Null
    } else {
        Invoke-ObsRequest "SetInputSettings" @{
            inputName = $SourceName
            inputSettings = $inputSettings
            overlay = $false
        } | Out-Null

        # Inputs are global OBS objects and can outlive the scene used by an
        # earlier interrupted test. Ensure an existing input is actually in
        # this test scene; otherwise it can decode in the background without
        # receiving video_tick(), and zero output frames look superficially
        # like a connected source.
        $sceneItem = Invoke-ObsRequest "GetSceneItemId" @{
            sceneName = $SceneName
            sourceName = $SourceName
        } -AllowFailure
        if (-not $sceneItem.requestStatus.result) {
            Invoke-ObsRequest "CreateSceneItem" @{
                sceneName = $SceneName
                sourceName = $SourceName
                sceneItemEnabled = $true
            } | Out-Null
        }
    }

    $listed = Invoke-VendorRequest "ListSources"
    Assert-Condition $listed.success "ListSources returned failure"
    $found = @($listed.sources | Where-Object name -eq $SourceName)
    Assert-Condition ($found.Count -eq 1) "ListSources did not return the test source"

    $url = Invoke-VendorRequest "GetStreamURL" @{
        sourceName = $SourceName
    }
    Assert-Condition $url.success "GetStreamURL returned failure"
    Assert-Condition ($url.url -eq $StreamUrl) "GetStreamURL returned the wrong URL"

    if (-not $SkipConnectionMutationChecks) {
        $missingUrl = Invoke-VendorRequest "SetStreamURL" @{
            sourceName = $SourceName
        }
        Assert-Condition (-not $missingUrl.success) `
            "SetStreamURL accepted a missing URL"

        $setUrl = Invoke-VendorRequest "SetStreamURL" @{
            sourceName = $SourceName
            url = $StreamUrl
        }
        Assert-Condition $setUrl.success "SetStreamURL returned failure"
        Assert-Condition ($setUrl.url -eq $StreamUrl) `
            "SetStreamURL returned the wrong URL"
    }

    $missing = Invoke-VendorRequest "GetStatus" @{
        sourceName = "__missing_smooth_media_audit_source__"
    }
    Assert-Condition (-not $missing.success) "Missing-source request unexpectedly succeeded"

    $status = Wait-ForPlayback -TimeoutSeconds 30
    $firstVideoFrames = [long]$status.videoFramesOut
    $firstAudioFrames = [long]$status.audioFramesOut

    if ($RecordDuringTest) {
        Invoke-ObsRequest "StartRecord" | Out-Null
        $script:RecordingStarted = $true
    }

    $stableDeadline = [DateTime]::UtcNow.AddSeconds($StableSeconds)
    $reconnectObserved = $false
    $recoveredAfterReconnect = $false
    do {
        Start-Sleep -Seconds 1
        $status = Get-SmoothStatus
        if ($status.reconnecting -or -not $status.active -or
            $status.state -ne "playing") {
            $reconnectObserved = $true
        } elseif ($reconnectObserved -and
                  [long]$status.videoFramesOut -gt 5) {
            $recoveredAfterReconnect = $true
        }
        if (-not $ExpectReconnectCycle -and
            -not $AllowTransientDisconnects) {
            Assert-Condition $status.active `
                "Source became inactive during stable playback"
            Assert-Condition ($status.state -eq "playing") `
                "Source left playing state"
        }
    } while ([DateTime]::UtcNow -lt $stableDeadline)

    if ($ExpectReconnectCycle) {
        Assert-Condition $reconnectObserved `
            "Expected reconnect transition was not observed"
        Assert-Condition $recoveredAfterReconnect `
            "Source did not recover after the reconnect transition"
    }
    if ($AllowTransientDisconnects) {
        Assert-Condition $status.active `
            "Source was not active after the transient-disconnect window"
        Assert-Condition ($status.state -eq "playing") `
            "Source was not playing after the transient-disconnect window"
    }
    if ($ExpectReconnectCycle -or $AllowTransientDisconnects) {
        Assert-Condition (
            [long]$status.videoFramesOut -gt 5
        ) "Video did not advance after reconnect"
        Assert-Condition (
            [long]$status.audioFramesOut -gt 5
        ) "Audio did not advance after reconnect"
    } else {
        Assert-Condition (
            [long]$status.videoFramesOut -gt $firstVideoFrames
        ) "Video frame counter did not advance"
        Assert-Condition (
            [long]$status.audioFramesOut -gt $firstAudioFrames
        ) "Audio frame counter did not advance"
    }

    for ($iteration = 1; $iteration -le $RestartIterations; $iteration++) {
        $before = Get-SmoothStatus
        $restart = Invoke-VendorRequest "RestartSource" @{
            sourceName = $SourceName
        }
        Assert-Condition $restart.success "RestartSource failed at iteration $iteration"
        Wait-ForRestartRecovery -TimeoutSeconds 20 `
            -BeforeVideoFrames ([long]$before.videoFramesOut) | Out-Null
    }

    if ($FinalStreamUrl) {
        $setFinalUrl = Invoke-VendorRequest "SetStreamURL" @{
            sourceName = $SourceName
            url = $FinalStreamUrl
        }
        Assert-Condition $setFinalUrl.success `
            "Unable to set the final stream URL"
    }

    # A state can remain "playing" with old, nonzero counters after media has
    # silently stalled. Prove current liveness over a short final window
    # instead of accepting historical output from earlier in the run.
    $livenessBefore = Get-SmoothStatus
    Start-Sleep -Seconds $FinalLivenessSeconds
    $final = Get-SmoothStatus
    Assert-Condition ($final.state -eq "playing" -and $final.active) `
        "Source was not playing during the final liveness check"
    Assert-Condition (
        [long]$final.videoFramesOut -gt
            [long]$livenessBefore.videoFramesOut
    ) "Video did not advance during the final liveness check"
    Assert-Condition (
        [long]$final.audioFramesOut -gt
            [long]$livenessBefore.audioFramesOut
    ) "Audio did not advance during the final liveness check"

    if ($script:RecordingStarted) {
        $recordResult = Invoke-ObsRequest "StopRecord"
        $script:RecordingOutputPath = $recordResult.responseData.outputPath
        $script:RecordingStarted = $false
    }

    $summary = [PSCustomObject]@{
        success = $true
        obsWebSocketVersion = $hello.d.obsWebSocketVersion
        hardwareDecoding = [bool]$HardwareDecoding
        reconnectCycleExpected = [bool]$ExpectReconnectCycle
        transientDisconnectsAllowed = [bool]$AllowTransientDisconnects
        connectionMutationChecksSkipped = [bool]$SkipConnectionMutationChecks
        recordingOutputPath = $script:RecordingOutputPath
        reconnectObserved = $reconnectObserved
        recoveredAfterReconnect = $recoveredAfterReconnect
        stableSeconds = $StableSeconds
        finalLivenessSeconds = $FinalLivenessSeconds
        restartIterations = $RestartIterations
        finalState = $final.state
        finalAudioFramesOut = [long]$final.audioFramesOut
        finalVideoFramesOut = [long]$final.videoFramesOut
        finalAvOffsetMs = [long]$final.avOffsetMs
        minAvOffsetMs = (
            $script:Observations |
                Measure-Object -Property avOffsetMs -Minimum
        ).Minimum
        maxAvOffsetMs = (
            $script:Observations |
                Measure-Object -Property avOffsetMs -Maximum
        ).Maximum
        observations = $script:Observations.Count
    }

    if ($ResultPath) {
        [PSCustomObject]@{
            summary = $summary
            observations = $script:Observations
        } | ConvertTo-Json -Depth 20 |
            Set-Content -Encoding UTF8 -Path $ResultPath
    }
    $summary | ConvertTo-Json -Depth 10
} catch {
    if ($ResultPath) {
        [PSCustomObject]@{
            summary = [PSCustomObject]@{
                success = $false
                error = $_.Exception.Message
                utc = [DateTime]::UtcNow.ToString("o")
                observations = $script:Observations.Count
            }
            observations = $script:Observations
        } | ConvertTo-Json -Depth 20 |
            Set-Content -Encoding UTF8 -Path $ResultPath
    }
    throw
} finally {
    if ($script:RecordingStarted -and $script:Identified -and
        $script:Socket -and
        $script:Socket.State -eq
            [Net.WebSockets.WebSocketState]::Open) {
        try {
            Invoke-ObsRequest "StopRecord" -AllowFailure | Out-Null
        } catch {
            Write-Warning "OBS recording cleanup failed: $($_.Exception.Message)"
        }
        $script:RecordingStarted = $false
    }
    if ($script:CheckpointWriter) {
        $script:CheckpointWriter.Dispose()
        $script:CheckpointWriter = $null
    }
    if ($CleanupOnExit -and $script:Identified -and
        $script:Socket -and
        $script:Socket.State -eq
            [Net.WebSockets.WebSocketState]::Open) {
        try {
            Invoke-ObsRequest "RemoveInput" @{
                inputName = $SourceName
            } -AllowFailure | Out-Null
            Invoke-ObsRequest "RemoveScene" @{
                sceneName = $SceneName
            } -AllowFailure | Out-Null
        } catch {
            Write-Warning "OBS test cleanup failed: $($_.Exception.Message)"
        }
    }
    if ($script:Socket -and
        $script:Socket.State -eq
            [Net.WebSockets.WebSocketState]::Open) {
        $null = $script:Socket.CloseAsync(
            [Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
            "audit complete",
            [Threading.CancellationToken]::None
        ).GetAwaiter().GetResult()
    }
    if ($script:Socket) {
        $script:Socket.Dispose()
    }
}
