# OBS Media Source Audio Stutter — Root Cause Analysis

> This file records the original root-cause investigation. The current
> implementation, thread ownership, lifecycle state machine, invariants, and
> acceptance gates are documented in
> [docs/architecture.md](docs/architecture.md) and
> [docs/testing.md](docs/testing.md). The completed hardening findings and
> remaining Windows/driver qualification are in
> [docs/reliability-audit.md](docs/reliability-audit.md).

## Issue Summary
GitHub Issue [#12724](https://github.com/obsproject/obs-studio/issues/12724): When using OBS Media Source with SRT/RTMP streams that arrive **slightly slower than realtime**, audio becomes choppy, stuttery, and won't recover without restarting the source.

## Root Cause

After reading the OBS media-playback code (`media.c`, `decode.c`, `obs-ffmpeg-source.c`), the root cause is a **wall-clock-coupled playback loop** that cannot tolerate streams slower than realtime.

### How OBS media playback works

1. **`mp_media_thread()`** is the main loop. It sleeps until the next frame's wall-clock deadline (`m->next_ns`), then outputs frames.

2. **`mp_media_sleep()`** sleeps based on `m->next_ns - os_gettime_ns()`. The wall clock is the master clock.

3. **`mp_media_can_play_frame()`** checks `d->frame_pts <= m->next_pts_ns` — a frame plays when its PTS is at or before the current playback position.

4. **Timestamps sent to OBS** are computed as:
   ```
   audio.timestamp = base_ts + frame_pts - start_ts + play_sys_ts - base_sys_ts
   ```
   This maps stream PTS into wall-clock time, anchored at `play_sys_ts` (the moment playback started).

### What breaks with slower-than-realtime streams

1. The playback thread's wall clock advances at 1.0x, but the stream delivers data at ~0.98-0.99x.

2. `av_read_frame()` inside `mp_media_next_packet()` **blocks** waiting for network data. While blocked, wall clock advances.

3. When data finally arrives, the frame PTS is "in the past" relative to `m->next_pts_ns`. The `can_play_frame` check passes immediately.

4. **Audio frames get dumped in bursts** — multiple frames output rapidly with wall-clock-derived timestamps that are bunched together. OBS's audio subsystem sees irregular timestamp spacing and produces stuttering.

5. **`mp_media_calc_next_ns()`** computes the next sleep delta from PTS differences. After a stall, this delta is tiny (back-to-back frames), causing rapid consumption of any buffer.

6. The drift between wall clock and stream clock **accumulates monotonically** — it never self-corrects. Over time, the playback thread spends more and more time blocked in `av_read_frame`, causing increasingly severe audio bursts.

### Why scene switching triggers it

`restart_on_activate` causes `mp_media_reset()` which resets `play_sys_ts = os_gettime_ns()`. This re-anchors the wall clock, but the stream is still slow, so drift immediately starts accumulating again. The reset can also land in the middle of a drift cycle, causing an abrupt timestamp discontinuity.

## Fix Strategy for Our Plugin

### 1. Decouple playback clock from wall clock
Use the **stream's own PTS as the master clock**. Track the relationship between stream PTS and wall time, and use the stream's pace to drive output timing.

### 2. Adaptive jitter buffer
Maintain a small audio frame buffer (~50-200ms configurable). Only output audio when the buffer has enough data. If the buffer runs low, **wait** instead of outputting silence or skipping.

### 3. Clock drift measurement and correction
- Measure stream_time_elapsed / wall_time_elapsed over a sliding window
- When drift is detected (stream slower than wall), gradually adjust output timestamps
- Use slight audio sample rate tweaking to absorb drift smoothly (e.g., output at 47900 Hz instead of 48000 Hz to slow down consumption by ~0.2%)

### 4. Buffer-aware frame gating
Never output a frame if doing so would starve the buffer. The check becomes:
```
can_play = frame_ready AND buffer_level > min_threshold
```
Instead of the original wall-clock-only check.

### 5. Smooth timestamp generation
Generate output timestamps from a monotonic counter based on actual audio samples output, not from wall-clock mapping. This ensures OBS's audio subsystem sees perfectly regular timestamps.
