# WebSocket Vendor API

obs-smooth-media exposes a vendor API through [obs-websocket](https://github.com/obsproject/obs-websocket) (v5+). All requests use the `obs-smooth-media` vendor name.

## Requirements

- **obs-websocket 5.x** installed and enabled (bundled with OBS 28+)
- **obs-smooth-media** plugin loaded

## Sending Requests

Use any obs-websocket client library. Vendor requests are sent via the `CallVendorRequest` message:

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "1",
    "requestData": {
      "vendorName": "obs-smooth-media",
      "requestType": "<RequestName>",
      "requestData": { ... }
    }
  }
}
```

### Python (obs-websocket-py)

```python
import obsws_python as obs

cl = obs.ReqClient(host="localhost", port=4455, password="secret")
resp = cl.call_vendor_request("obs-smooth-media", "SetStreamURL", {
    "sourceName": "My Source",
    "url": "srt://host:port"
})
print(resp.response_data)
```

### Rust (obws)

```rust
let resp = client.general()
    .call_vendor_request::<serde_json::Value>(
        "obs-smooth-media",
        "RestartSource",
        &serde_json::json!({ "sourceName": "My Source" }),
    ).await?;
```

---

## Requests

### SetStreamURL

Change the stream URL of a source. Triggers a full reconnect.

**Request:**

| Field        | Type   | Required | Description                        |
|--------------|--------|----------|------------------------------------|
| `sourceName` | string | yes      | Name of the Smooth Media Source    |
| `url`        | string | yes      | New stream URL (SRT, RTMP, etc.)   |

**Response:**

| Field     | Type   | Description                  |
|-----------|--------|------------------------------|
| `success` | bool   | `true` on success            |
| `url`     | string | The URL that was set         |
| `error`   | string | Error message (on failure)   |

**Example:**

```json
{
  "sourceName": "Camera Feed",
  "url": "srt://192.168.1.10:9000?mode=caller"
}
```

---

### GetStreamURL

Get the current stream URL of a source.

**Request:**

| Field        | Type   | Required | Description                        |
|--------------|--------|----------|------------------------------------|
| `sourceName` | string | yes      | Name of the Smooth Media Source    |

**Response:**

| Field     | Type   | Description                  |
|-----------|--------|------------------------------|
| `success` | bool   | `true` on success            |
| `url`     | string | Current stream URL           |
| `error`   | string | Error message (on failure)   |

---

### GetStatus

Get the current playback status and diagnostics for a source.

**Request:**

| Field        | Type   | Required | Description                        |
|--------------|--------|----------|------------------------------------|
| `sourceName` | string | yes      | Name of the Smooth Media Source    |

**Response:**

| Field            | Type   | Description                                        |
|------------------|--------|----------------------------------------------------|
| `success`        | bool   | `true` on success                                  |
| `state`          | string | Media state: `none`, `playing`, `opening`, `buffering`, `paused`, `stopped`, `ended`, `error` |
| `active`         | bool   | Whether the decoder thread is running              |
| `reconnecting`   | bool   | Whether a reconnect is in progress                 |
| `url`            | string | Current stream URL                                 |
| `audioFramesOut` | int    | Total audio frames output to OBS                   |
| `videoFramesOut` | int    | Total video frames output to OBS                   |
| `avOffsetMs`     | int    | Audio-video wall offset in milliseconds            |
| `error`          | string | Error message (on failure)                         |

---

### RestartSource

Force a full stop + reconnect cycle on a source. This is the equivalent of calling `SetInputSettings` with empty JSON on a standard media source — it re-applies the current settings, which tears down the decoder thread and starts a fresh connection.

Use this as a "fix" when a source is stuck, has drifted, or needs a clean reset.

**Request:**

| Field        | Type   | Required | Description                        |
|--------------|--------|----------|------------------------------------|
| `sourceName` | string | yes      | Name of the Smooth Media Source    |

**Response:**

| Field     | Type   | Description                  |
|-----------|--------|------------------------------|
| `success` | bool   | `true` on success            |
| `error`   | string | Error message (on failure)   |

**Example:**

```json
{ "sourceName": "Camera Feed" }
```

---

### ListSources

List all Smooth Media Source instances in the current scene collection.

**Request:** *(no parameters)*

**Response:**

| Field     | Type   | Description                       |
|-----------|--------|-----------------------------------|
| `success` | bool   | `true` on success                 |
| `sources` | array  | Array of `{ name, url }` objects  |

**Example response:**

```json
{
  "success": true,
  "sources": [
    { "name": "Camera Feed", "url": "srt://192.168.1.10:9000" },
    { "name": "Backup", "url": "rtmp://live.example.com/stream" }
  ]
}
```

---

## Error Handling

All requests return `"success": false` with an `"error"` string on failure. Common errors:

| Error                            | Cause                                      |
|----------------------------------|--------------------------------------------|
| `Missing 'sourceName' parameter` | `sourceName` was empty or not provided     |
| `Missing 'url' parameter`        | `url` was empty or not provided            |
| `Source not found`                | No Smooth Media Source with that name exists|
| `Source data unavailable`         | Source exists but internal data is null     |
