# netsim — bad-network test streamer

Publishes a **test pattern** (a moving image with an on-screen timer, plus a
steady audio tone) with IRL-style network impairments baked in, so you can
reproduce nasty conditions on demand, point the **Smooth Media Source** at it,
and send the OBS log.

Why a tone + timer: a pure tone makes *any* audio stutter, click, gap, or
pitch wobble instantly obvious by ear, and the on-screen timer makes video
freezes / A-V drift obvious by eye.

## Requirements

A standalone **ffmpeg** (not the one inside OBS). On the Windows server grab the
"full" build from <https://www.gyan.dev/ffmpeg/builds/> (it includes SRT **and**
RIST). On macOS: `brew install ffmpeg`. Verify: `ffmpeg -version`, and for RIST
`ffmpeg -protocols | findstr rist`. ffmpeg **6.1+** recommended (for `burst`).

## Two modes

```
netsim.sh  <scenario> <destination>      # macOS / Linux
netsim.bat <scenario> <destination>      # Windows
```

**1. Push to a remote server (what you usually want).** Give a full publish URL
and netsim pushes the impaired stream to your ingest server; then point the
source at that server's **playback** URL. This exercises the real path through
your server.

```
netsim.bat slow  rtmp://172.238.7.67:1935/publish/test
netsim.bat burst "srt://172.238.7.67:8282?streamid=publish/live/test&mode=caller"
netsim.bat stall rist://172.238.7.67:9001
```
> Quote any push URL containing `&` (Windows cmd will otherwise split it).
> The source then plays e.g. `rtmp://172.238.7.67:1935/publish/test`.

**2. Local listener (self-contained, no server).** Give a bare protocol and
netsim listens; point the source straight at it.

```
netsim.sh slow srt
# -> source plays srt://<this-host>:9000   (127.0.0.1 if same box)
```
Local listener ports: RTMP **1936** (avoids a real nginx on 1935), SRT **9000**,
RIST **9001**.

## Scenarios

| scenario   | simulates                                       | what to watch in the `DBG` log                  |
|------------|-------------------------------------------------|-------------------------------------------------|
| `clean`    | perfect 1.0x delivery (baseline)                | `rate≈1.000`, `under=0`, `sr_chg` 0–1           |
| `slow`     | feed ~3% under real time (classic IRL case)     | `rate≈0.97`, buffer holds, **0 underruns**      |
| `veryslow` | ~7% under real time (extreme)                   | `rate≈0.93`, still no stutter                   |
| `fast`     | feed ~3% over real time                         | `rate≈1.03`, no runaway buffering               |
| `burst`    | server flushes a backlog at connect             | `initial trim` fires, no rate spike / clicks    |
| `stall`    | stream drops every ~25s, returns after ~6s      | clean reconnect, fast recovery, bounded `drop`  |
| `latency`  | (SRT listener) deep 2s receive buffer           | larger steady `buf`, still smooth               |

Turn on **Verbose Debug Logging** in the source's Advanced settings first, so
the log has the per-second `DBG` lines.

## Tuning (env vars)

`SIZE` (1920x1080), `FPS` (60), `VB` (4000k), `AB` (160k), `TONE_HZ` (440),
`STALL_ON`/`STALL_OFF` (25/6 s), `RTMP_PORT`/`SRT_PORT`/`RIST_PORT`,
`RTMP_APP`/`RTMP_KEY`, `SRT_LATENCY`. Drop to `SIZE=1280x720 FPS=30` if the box
is busy.

## True jitter / packet loss / added RTT

ffmpeg shapes the *send pace* (above) but can't inject real jitter/loss/RTT. For
those, add an impairment layer on the test port:

- **Windows:** [clumsy](https://jagt.github.io/clumsy/) — filter the test port
  (e.g. `udp and port 9000`) and enable Lag (~80 ms), Drop (~2%), Jitter. Ideal
  for SRT/RIST (UDP).
- **Linux:** `tc qdisc add dev <iface> root netem delay 80ms 40ms loss 2%`.

Combine, e.g., `slow` + clumsy jitter, for a true worst-case bonded-cellular link.

## Capturing the log

OBS → **Help → Log Files → Upload Current Log File**, and send it. The relevant
lines are `[Smooth Media Source ...] DBG ...` plus any `underrun` /
`sample-rate change` / `overflow` / `Failed to open` / `Reconnect` entries.
