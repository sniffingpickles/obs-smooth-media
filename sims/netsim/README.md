# netsim — bad-network test streamer

Publishes a **test pattern** (a moving image with an on-screen timer, plus a
steady audio tone) as an **SRT or RTMP listener**, with IRL-style network
impairments baked in. Point the **Smooth Media Source** at it, reproduce the
nasty conditions on demand, and send the OBS log.

Why a tone + timer: a pure tone makes *any* audio stutter, click, gap, or
pitch wobble instantly obvious by ear, and the on-screen timer makes video
freezes / A-V drift obvious by eye.

## Requirements

A standalone **ffmpeg** (not the one inside OBS). On the Windows server grab a
build from <https://www.gyan.dev/ffmpeg/builds/> (the "full" build has SRT +
RIST). On macOS: `brew install ffmpeg`. Check it works: `ffmpeg -version`.

ffmpeg **6.1+** recommended (needed for the `burst` scenario).

## How it works

The script runs ffmpeg as the **listener/server** and the plugin connects to
it as the client:

- **RTMP:** ffmpeg listens on a port (default **1936**, to avoid clashing with
  a real nginx-rtmp on 1935). Point the source at `rtmp://<host>:1936/live/test`.
- **SRT:** ffmpeg listens on a UDP port (default **9000**). Point the source at
  `srt://<host>:9000`.

`<host>` is `127.0.0.1` if OBS and the script run on the same box, otherwise the
server's IP (open the port in the firewall).

## Usage

```
# Linux / macOS
./netsim.sh <rtmp|srt> <scenario>

# Windows
netsim.bat <rtmp|srt> <scenario>
```

Example:

```
./netsim.sh srt slow
netsim.bat rtmp stall
```

The script prints the exact URL to paste into the source.

## Scenarios

| scenario   | what it simulates                              | what to watch in the log (`DBG ...`)            |
|------------|------------------------------------------------|-------------------------------------------------|
| `clean`    | perfect 1.0x delivery (baseline)               | `rate≈1.000`, `under=0`, `sr_chg` stays 0–1     |
| `slow`     | feed ~3% under real time (the classic IRL case)| `rate≈0.97`, buffer holds, **0 underruns**      |
| `veryslow` | ~7% under real time (extreme)                  | `rate≈0.93`, still no stutter                   |
| `fast`     | feed ~3% over real time                        | `rate≈1.03`, no runaway buffering               |
| `burst`    | server flushes a backlog at connect            | `initial trim` fires, no rate spike / clicks    |
| `stall`    | stream drops every ~25s, returns after ~6s     | clean reconnect, fast recovery, bounded `drop`  |
| `latency`  | (SRT) deep 2s receive buffer                   | larger steady `buf`, still smooth               |

Turn on **Verbose Debug Logging** in the source's Advanced settings first, so
the log has the per-second `DBG` lines.

## True jitter / packet loss / added RTT

ffmpeg can shape the *send pace* (the scenarios above), but it can't inject
real jitter, packet loss, or round-trip latency. For those, run an impairment
layer in front of the listener port:

- **Windows:** [clumsy](https://jagt.github.io/clumsy/) — filter on the test
  port (e.g. `udp and outbound and port 9000`) and enable Lag (e.g. 80 ms),
  Drop (e.g. 2%), and Jitter. Great for SRT (UDP).
- **Linux:** `tc qdisc add dev <iface> root netem delay 80ms 40ms loss 2%`.

Combine, e.g., `slow` + clumsy jitter, for a true worst-case IRL link.

## Capturing the log

OBS → **Help → Log Files → Upload Current Log File** (or **View Current Log**),
and send it over. The interesting lines are `[Smooth Media Source ...] DBG ...`
and any `underrun` / `sample-rate change` / `overflow` / `Failed to open` /
`Reconnect` entries.
