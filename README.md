# Smooth Media Source for OBS

> **Beta Release** — This plugin is actively being developed and tested. It's stable for daily use, but we're still refining things. Feedback and bug reports are welcome!

A drop-in OBS Studio plugin that **fixes audio stutter, choppy playback, and A/V desync** when using RTMP, SRT, or RIST live streams as a media source in OBS Studio. Built for **IRL streamers**, **multi-cam setups**, **remote guest feeds**, and anyone pulling live video into OBS over a network.

**Keywords:** OBS audio stutter fix, OBS SRT audio choppy, OBS RTMP audio desync, OBS media source crackling, OBS live stream audio fix, OBS plugin smooth playback, IRL streaming OBS audio, OBS network stream stutter

## Why?

OBS's built-in Media Source locks its playback clock to your system's wall clock. When a live stream delivers data even slightly slower than realtime — which is common with software encoders, the FFmpeg `-re` flag, or normal network jitter — audio frames pile up and get dumped in bursts. The result is choppy, stuttering audio that won't recover without manually restarting the source.

This is a [known OBS issue](https://github.com/obsproject/obs-studio/issues/12724) with no built-in fix.

**Smooth Media Source** solves this by decoupling playback from the wall clock and using the stream's own timing to drive output.

## Installation

1. Download the latest release from the [Releases page](https://github.com/sniffingpickles/obs-smooth-media/releases)
2. Close OBS Studio
3. Extract the zip into your OBS installation folder (e.g. `C:\Program Files\obs-studio`)
   - This places the plugin DLL into `obs-plugins/64bit/` and locale files into `data/obs-plugins/`
4. Start OBS Studio

> **Requires OBS Studio 32.0.x (Windows 64-bit)**

## Usage

1. In OBS, add a new source and select **Smooth Media Source**
2. Enter your stream URL (RTMP, SRT, or RIST)
3. That's it — audio and video will play back smoothly without stutter

### Source Settings

| Setting | Default | Description |
|---------|---------|-------------|
| **Stream URL** | — | Any RTMP, SRT, or RIST stream URL |
| **Input Format** | *(auto)* | Force a specific container format (e.g. `mpegts`), usually not needed |
| **Reconnect Delay** | 10 s | How long to wait before retrying after a disconnect |
| **Hardware Decoding** | Off | Use GPU-accelerated decoding (if available) |
| **FFmpeg Options** | — | Advanced: additional FFmpeg demuxer/decoder options |

The plugin automatically handles network buffering, audio jitter absorption, and clock drift correction — no manual tuning required.

## How It Works

The plugin uses three techniques to keep playback smooth:

- **Clock Drift Tracking** — Continuously measures how fast the stream delivers data relative to wall time using an EMA-smoothed ratio over a sliding window.

- **Audio Jitter Buffer** — Holds 80ms of audio before starting playback. Absorbs short-term network timing irregularities so they never reach the output.

- **Adaptive Sample Rate Correction** — If the stream is consistently slower or faster than realtime (beyond a 4% deadzone sustained for 3+ seconds), the declared audio sample rate is nudged so OBS consumes audio at the stream's actual pace. No resampling artifacts — just seamless pacing.

Video frame timestamps use PTS-delta stepping from the encoder for perfectly even frame spacing (e.g. exactly 16.667ms for 60fps), with a gentle drift correction to stay aligned with audio over long sessions.

## Supported Protocols

| Protocol | Example URL |
|----------|-------------|
| **RTMP** | `rtmp://server:1935/app/stream` |
| **SRT** | `srt://server:port?streamid=...` |
| **RIST** | `rist://server:port` |

Any other protocol supported by FFmpeg's `avformat_open_input` should also work.

## Building from Source

<details>
<summary>Click to expand build instructions</summary>

### Prerequisites

- CMake 3.16+
- Visual Studio 2022 (MSVC)
- OBS Studio source headers (32.0.x)
- FFmpeg development libraries

### Quick Build

```powershell
git clone https://github.com/sniffingpickles/obs-smooth-media.git
cd obs-smooth-media

# Download dependencies (OBS portable + headers + FFmpeg)
.\setup-windows-deps.ps1

# Build
.\build-windows.bat "deps\obs-studio" "deps\ffmpeg"
```

The built plugin will be in `build/Release/`.

### Manual CMake Build

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 \
  -DOBS_DIR="C:/path/to/obs-studio" \
  -DFFMPEG_DIR="C:/path/to/ffmpeg"

cmake --build build --config Release
```

### Project Structure

```
src/
├── plugin-main.c          # OBS module entry point
├── smooth-media-source.c  # Source implementation (callbacks, timestamps, jitter buffer)
├── smooth-media-source.h
├── stream-decoder.c       # FFmpeg demuxing and decoding
├── stream-decoder.h
├── audio-buffer.c         # Audio jitter buffer
├── audio-buffer.h
├── clock-tracker.c        # Stream-vs-wall-clock drift measurement
└── clock-tracker.h
```

</details>

## Community & Support

This plugin was developed and tested by **[IRLhosting.com](https://irlhosting.com)** with the help of AI.

- **Discord:** [discord.gg/IRLtools](https://discord.gg/IRLtools) — Get help, report bugs, or request features
- **Website:** [irlhosting.com](https://irlhosting.com) — IRL streaming infrastructure and tools

If this plugin helped you, consider joining the community and sharing your experience!

## License

GNU General Public License v2.0 — see [LICENSE](LICENSE) for details.
