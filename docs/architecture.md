# Architecture and Reliability Invariants

## Purpose

Smooth Media Source is a live-network input for OBS. It exists because a
wall-clock-driven player behaves badly when an encoder/network delivers media
at 0.97–1.03× realtime or in bursts: audio is emitted in clumps, the OBS audio
queue grows or starves, and audio/video timestamps drift apart.

The plugin instead:

1. decodes packets as they arrive;
2. measures the stable relationship between stream PTS and wall time;
3. holds decoded audio in an adaptive jitter buffer;
4. drains audio at the measured delivery pace;
5. gives OBS monotonic timestamps whose spacing matches the declared playback
   rate; and
6. delays video by the actual queued audio depth so matched content remains
   lip-synced during refill/overfill.

## Data path

```text
RTMP / SRT / RIST
        |
        v
FFmpeg demux + decode (media thread)
        |                         |
        | video                   | audio
        v                         v
format validation          delivery-rate tracker
timestamp pacing           adaptive jitter buffer
        |                         |
        |                         v
        |                 paced drain (OBS video_tick)
        |                         |
        +------------+------------+
                     v
           OBS async video/audio APIs
```

The decoder owns FFmpeg packet/frame objects. Callbacks either copy audio into
the jitter buffer or synchronously hand video to OBS, which copies async frame
data before the callback returns.

## Thread ownership

| State | Owner / synchronization |
|---|---|
| Decoder and FFmpeg contexts | Media thread only |
| `kill`, `active`, first-audio and notification flags | OBS atomic helpers |
| Source start/stop/settings/reconnect deadline | `lifecycle_mutex` |
| Audio ring and adaptive statistics | Audio-buffer mutex |
| Clock history/rate/drift | Clock mutex |
| Shared audio-controller/video pace fields | `controller_mutex` |
| Published video timestamp/counter | `timing_mutex` |
| Media state and reconnect attempt count | `state_mutex` |

There is deliberately no reconnect worker. `video_tick` owns the reconnect
deadline and launches a normal media thread when it expires. Network open and
stream probing remain off the OBS thread, while buffer reset and source
lifecycle changes remain serialized.

The stop path never dereferences a shared decoder pointer. It atomically sets
`kill`; FFmpeg's interrupt callback observes that flag during
`avformat_open_input`, stream probing, and reads. The lifecycle owner then
joins the media thread.

OBS media-started/ended signals are queued as atomic flags by the media thread
and emitted after releasing `lifecycle_mutex`, because OBS signals are
synchronous and handlers may query the source.

## Lifecycle

```text
STOPPED --start--> OPENING --open success--> PLAYING
                         \--failure/EOF----> ENDED
ENDED --delay expires--> OPENING
any state --hide/stop/decoder-setting update/destroy--> STOPPED
```

- Changing URL, forced format, FFmpeg options, or hardware-decode mode rebuilds
  the decoder.
- Playback-only settings apply live.
- With **Close When Inactive** enabled, hide/deactivate stops the connection.
- With it disabled, reconnect remains enabled even while the source is hidden.
- A blocked open is cancellation-safe; source destruction does not wait for
  the network timeout.

## Hard invariants

- Audio-buffer memory and queue indices change only under its mutex.
- A failed allocation does not modify ring indices or discard the oldest
  frame.
- Buffer depth and slot count are bounded.
- Unusually large audio allocations are released rather than cached across all
  ring slots.
- Small 2.5 ms audio frames can still reach the maximum adaptive target.
- Audio data returned to the consumer is isolated from concurrent producer
  pushes until the next pop/reset.
- Stream/wall timestamp discontinuities reset the clock measurement epoch.
- Audio and video timestamps never move backwards.
- Unsupported audio layouts/formats, invalid video dimensions/strides,
  oversized audio frames, and malformed FFmpeg options are rejected before
  reaching OBS.
- A decoder `EAGAIN` drains and retries the same packet rather than silently
  dropping it.
- A demuxer read `EAGAIN` is a temporary live-stream gap, not EOF; the media
  thread yields briefly without destroying transport state.
- Recoverable corrupt packet/frame errors are dropped and rate-limited so a
  later clean keyframe can resume; allocation failure remains fatal.
- Open and stream probing have an interrupt-callback deadline and observe the
  source's atomic teardown request.
- End-of-file flushes buffered decoder frames.

## Timing controller

The adaptive audio target is:

```text
80 ms floor + 3 × smoothed inter-arrival jitter + underrun credit
```

It is capped at 600 ms. The hard drop ceiling is target + 250 ms, capped at
1.2 s. One starvation episode adds one unit of underrun credit; repeated OBS
ticks during the same outage do not inflate the credit or diagnostics.

The clock uses a five-second window with 2,048 history slots, sufficient for
small codec frames. Connect/stall recovery is excluded from measurement until
delivery settles.

Audio timestamps advance by:

```text
samples / declared_sample_rate
```

Video PTS deltas are scaled by the same published playback ratio. Video delay
uses `max(adaptive target, actual queued audio)` within the hard ceiling.

## Limits

No native plugin can prove it will never crash under every OBS, driver,
codec, or third-party filter combination. The automated suite establishes
memory/thread correctness for isolated controller paths and exercises real
FFmpeg decoding/cancellation. Final release qualification still requires the
Windows OBS soak and bad-network matrix in [testing.md](testing.md). The
completed source audit and remaining host-level risks are recorded in
[reliability-audit.md](reliability-audit.md).
