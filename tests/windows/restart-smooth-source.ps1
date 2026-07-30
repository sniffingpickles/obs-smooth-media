param(
    [string]$WebSocketUri = "ws://127.0.0.1:4455",
    [Parameter(Mandatory = $true)]
    [string]$SourceName
)

$ErrorActionPreference = "Stop"
$socket = $null

function Send-WebSocketJson {
    param([Parameter(Mandatory = $true)]$Value)

    $json = $Value | ConvertTo-Json -Depth 20 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    $null = $script:socket.SendAsync(
        $segment,
        [Net.WebSockets.WebSocketMessageType]::Text,
        $true,
        [Threading.CancellationToken]::None
    ).GetAwaiter().GetResult()
}

function Receive-WebSocketJson {
    $buffer = New-Object byte[] 65536
    $stream = New-Object IO.MemoryStream
    try {
        do {
            $segment = [ArraySegment[byte]]::new($buffer)
            $result = $script:socket.ReceiveAsync(
                $segment,
                [Threading.CancellationToken]::None
            ).GetAwaiter().GetResult()
            if ($result.MessageType -eq
                [Net.WebSockets.WebSocketMessageType]::Close) {
                throw "OBS WebSocket closed unexpectedly"
            }
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)

        return (
            [Text.Encoding]::UTF8.GetString($stream.ToArray()) |
                ConvertFrom-Json
        )
    } finally {
        $stream.Dispose()
    }
}

function Invoke-ObsRequest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RequestType,
        $RequestData = @{}
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

    if (-not $message.d.requestStatus.result) {
        $status = $message.d.requestStatus
        throw "$RequestType failed ($($status.code)): $($status.comment)"
    }
    return $message.d
}

try {
    $script:socket = New-Object Net.WebSockets.ClientWebSocket
    $null = $script:socket.ConnectAsync(
        [Uri]$WebSocketUri,
        [Threading.CancellationToken]::None
    ).GetAwaiter().GetResult()

    $hello = Receive-WebSocketJson
    if ($hello.op -ne 0) {
        throw "Expected OBS WebSocket Hello"
    }
    Send-WebSocketJson @{
        op = 1
        d = @{
            rpcVersion = 1
            eventSubscriptions = 0
        }
    }
    $identified = Receive-WebSocketJson
    if ($identified.op -ne 2) {
        throw "OBS WebSocket identification failed"
    }

    $result = Invoke-ObsRequest "CallVendorRequest" @{
        vendorName = "obs-smooth-media"
        requestType = "RestartSource"
        requestData = @{
            sourceName = $SourceName
        }
    }
    $response = $result.responseData.responseData
    if (-not $response.success) {
        throw "RestartSource failed: $($response.error)"
    }

    [PSCustomObject]@{
        success = $true
        sourceName = $SourceName
        utc = [DateTime]::UtcNow.ToString("o")
    } | ConvertTo-Json
} finally {
    if ($script:socket -and
        $script:socket.State -eq
            [Net.WebSockets.WebSocketState]::Open) {
        $null = $script:socket.CloseAsync(
            [Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
            "restart complete",
            [Threading.CancellationToken]::None
        ).GetAwaiter().GetResult()
    }
    if ($script:socket) {
        $script:socket.Dispose()
    }
}
