# obs-smooth-media — Stutter-Free RTMP/SRT Media Source for OBS

An OBS Studio plugin that provides a new "Smooth Media Source" specifically designed to fix audio stutter when playing back RTMP and SRT streams that deliver data slightly slower than realtime.

## The Problem

OBS's built-in Media Source ties its playback clock to the system wall clock. When a live stream delivers data at 0.98x realtime (common with software encoders, `-re` flag, or network jitter), the wall clock races ahead of the stream. Audio frames pile up and get dumped in bursts, causing choppy/stuttering audio that won't recover without restarting.

See: [obsproject/obs-studio#12724](https://github.com/obsproject/obs-studio/issues/12724)

## The Fix

This plugin implements three key techniques:

1. **Clock Drift Tracking** — Measures the actual stream rate relative to wall time and adjusts output timestamps to match the stream's pace, not the wall clock.

2. **Audio Jitter Buffer** — Buffers audio frames (configurable, default 80ms) before outputting. Absorbs short-term timing irregularities. If buffer runs low, it waits for more data instead of outputting with bad timing.

3. **Sample Rate Correction** — When stream drift is detected, the declared audio sample rate is slightly adjusted (e.g., 48000→47040 Hz for a 0.98x stream) so OBS plays audio at the stream's actual pace. No resampling artifacts — just smooth playback.

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| Stream URL | — | RTMP, SRT, or any FFmpeg-supported URL |
| Network Buffer | 2 MB | FFmpeg input buffer size |
| Audio Jitter Buffer | 80 ms | Minimum audio buffering before playback starts |
| Max Audio Buffer | 500 ms | Maximum buffer before old frames are dropped |
| Sync to Stream Clock | On | Enable clock drift correction (the anti-stutter feature) |
| Reconnect Delay | 10 s | Delay before reconnect attempt on disconnect |
| Hardware Decoding | Off | Use GPU-accelerated decoding |
| FFmpeg Options | — | Additional FFmpeg demuxer options |

## Building (Windows)

### Prerequisites
- CMake 3.16+
- Visual Studio 2022 (or compatible MSVC)
- OBS Studio (installed or portable)
- FFmpeg development libraries (headers + import libs)

### Steps

```bash
# Clone this repo
git clone <this-repo> obs-smooth-media
cd obs-smooth-media

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 \
  -DOBS_DIR="C:/path/to/obs-studio" \
  -DFFMPEG_DIR="C:/path/to/ffmpeg"

# Build
cmake --build build --config Release

# Install (copies DLL to OBS plugins directory)
cmake --install build --config Release
```

### Using with Portable OBS
Point `OBS_DIR` to the portable OBS directory. The plugin DLL goes into `obs-plugins/64bit/` and locale data into `data/obs-plugins/obs-smooth-media/`.

## Architecture

```
src/
├── plugin-main.c          # OBS module entry point
├── smooth-media-source.c  # Main source implementation (OBS callbacks, output logic)
├── smooth-media-source.h
├── stream-decoder.c       # FFmpeg demuxing and decoding
├── stream-decoder.h
├── audio-buffer.c         # Jitter buffer for audio frames
├── audio-buffer.h
├── clock-tracker.c        # Stream-vs-wall-clock drift measurement
└── clock-tracker.h
```

## License

ISC License (same as OBS media-playback code)
