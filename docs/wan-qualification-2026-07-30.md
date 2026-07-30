# WAN Transport Qualification — 2026-07-30

## Scope

This qualification used a public Ubuntu 24.04 VM as an impaired SRT, RTMP, or
RIST sender and the isolated OBS 32.0.4 portable environment on the Windows
qualification host as the receiver. Every result came from the isolated
v1.4.13 test DLL; the installed production DLL was not replaced.

Each Linux sender ran inside a dedicated network namespace. Only the selected
transport crossed a virtual Ethernet pair with `tc netem` on both directions.
SSH and unrelated host traffic were outside the shaped path. RTMP and RIST
used separate namespaces, veth pairs, ports, logs, and qdiscs and could run at
the same time. The baseline TCP connection time between the hosts was about
250 ms before added impairment.

The media fixture was:

- 1280×720 at 30 fps;
- H.264 at approximately 3.5 Mbps;
- 48 kHz stereo AAC at 160 Kbps;
- approximately 4.1 Mbps after MPEG-TS overhead; and
- SRT caller/listener latency configured to 3,000 ms.

[`tests/network/wan-netem.sh`](../tests/network/wan-netem.sh) creates and
removes the isolated namespace. The FFmpeg listener is supervised so every
transport disconnect re-arms a fresh listener. This is necessary because a
one-shot FFmpeg SRT listener exits when its peer times out.

## Impairment matrix

The 900-second observer recorded 903 one-second status samples while
[`run-hostile-matrix.sh`](../tests/network/run-hostile-matrix.sh) applied:

| Stage | Bidirectional impairment |
|---|---|
| Mild transition | 80±40 ms one-way delay and 1% correlated loss |
| High RTT | 750±250 ms one-way delay and 2% correlated loss |
| Jitter/reorder | 220±200 ms delay, 5% correlated loss, 1% duplication, and 25% reordering |
| Loss burst | 120±80 ms delay and 20% highly correlated loss |
| Collapse | 250±150 ms delay, 5% loss, and a 384 Kbps ceiling |
| Extreme | 1,000±500 ms delay, 30% correlated loss, 2% duplication, 25% reordering, and a 512 Kbps ceiling |
| Spikes | Five 5-second high-RTT transitions separated by 10-second recovery windows |
| Bitrate swings | Five 5-second collapse transitions separated by 10-second impaired recovery windows |
| Blackout | 40 seconds of 100% loss, longer than the SRT transport timeout |

The test deliberately changed qdiscs instantly. A transition therefore also
creates a short packet-order discontinuity, like an abrupt cellular path
handoff, rather than only a pre-existing steady network condition.

## SRT hostile-matrix result

The end-to-end harness passed:

- OBS remained alive and its WebSocket control plane remained responsive;
- 28 logged disconnects were matched by 28 successful reconnects;
- the harness observed reconnect state and subsequent audio/video recovery;
- the final 90-second clean window was playing for all 90 observations;
- the final state was `playing`, with 9,053 audio and 5,807 video frames in
  the last decoder epoch;
- the longest no-video interval was 141.6 seconds while several hostile
  stages, rapid transitions, and the deliberate blackout continued;
- after the final blackout cleared, playback returned in about 1.9 seconds;
- OBS working set ended 79.5 MB below its starting sample;
- private bytes changed from 171.5 MB to 178.4 MB while remaining bounded
  between 121.3 and 192.8 MB;
- handles changed from 1,572 to 1,545 and threads from 32 to 25; and
- clean post-stress shutdown completed in 527 ms with
  `Number of memory leaks: 0`.

The harsh stages did not remain visually smooth; they intentionally removed
more media than the transport could deliver. For example, the 45-second
384 Kbps mixed-collapse stage remained in `playing` state but delivered only
128 additional video frames. The plugin cannot synthesize missing live media.
Its reliability requirement is bounded behavior, correct state/recovery, no
deadlock, and no unbounded resource growth.

The status `avOffsetMs` peaked at 21.8 seconds while starved media timestamps
were stale and returned to −171 ms after recovery. This field is the difference
between the most recently scheduled OBS audio and video timestamps. It is not
a matched-content lip-sync measurement; video is intentionally scheduled ahead
by the current audio-buffer depth.

## SRT isolated bitrate-cliff result

[`run-bitrate-cliffs.sh`](../tests/network/run-bitrate-cliffs.sh) removed
latency, jitter, and loss from the equation and varied only the rate ceiling.
The 300-second observer recorded 302 samples:

- all 302 were active and `playing`;
- no reconnect occurred;
- the longest observation interval without a new video frame was one second;
- the 2 Mbps/20-second stage delivered 285 video frames;
- the 1 Mbps/15-second stage delivered 155 video frames;
- the 384 Kbps/10-second stage delivered 25 video frames;
- three additional five-second 384 Kbps cliffs also remained playing;
- each clean recovery window returned near the expected 30 fps/48 kHz output
  rate while trimming or dropping stale backlog to keep latency bounded; and
- the final 45-second clean window delivered 1,323 video frames and 2,065
  AAC frames.

The bitrate run ended `playing` without a reconnect. Working set was
essentially flat (−0.6 MB), handles decreased by five, and threads decreased
by one.

## RIST qualification

The first RIST implementation did not pass. FFmpeg's libRIST protocol returns
`EAGAIN` during ordinary receive gaps, but the decoder treated that as a fatal
read, destroyed the transport, and repeatedly reopened it. Loss-corrupted
codec packets also tore down the entire transport. Those cycles produced stuck
open/reconnect behavior and one real Windows heap-corruption crash
(`0xc0000374` in `ntdll.dll`) on the earlier build.

The decoder now:

- treats `EAGAIN` as a live gap and yields briefly;
- keeps one reusable packet;
- drops and rate-limits recoverable corrupt packet/frame errors while treating
  `ENOMEM` as fatal;
- enforces a 35-second interrupt-callback deadline around open and probe; and
- logs the protocol and error without logging secret-bearing URLs.

A focused sequence covering high RTT, the extreme profile, blackout, and clean
recovery then completed 130 seconds in `playing` state without reconnecting.
Final counters were 11,366 audio and 7,331 video frames; corrupt audio packets
were logged and later clean frames continued decoding.

The full 900-second RIST hostile matrix also passed:

- all 900 status observations remained active and `playing`;
- there was no reconnect or counter reset;
- the deliberate blackout produced the longest no-audio/no-video interval,
  39.1/40.1 seconds;
- the final 60 seconds advanced 2,771 audio and 1,774 video frames;
- final counters were 33,538 audio and 22,338 video;
- final `avOffsetMs` was −208 ms (observed range −4,823 to +37,736 ms during
  starvation/recovery); and
- correct cross-session shutdown completed in 628 ms with zero OBS-reported
  memory leaks.

After the last initialization/OOM hardening change, the final DLL also ran
RIST clean for 903 seconds alongside the RTMP hostile matrix. All 901
observations remained active and `playing`, with no reconnect or counter
reset, a maximum 3.02-second observation interval between counter advances,
final counters of 53,438 audio and 34,198 video, and an `avOffsetMs` range of
−344 to −248 ms.

## RTMP qualification

An earlier isolated 900-second RTMP hostile run survived 28
disconnect/reconnect cycles and recovered to a healthy final state. It also
exposed RTMP's expected TCP-backlog behavior: `avOffsetMs` temporarily reached
+27.263 seconds during severe rate starvation before stale media was bounded
and the source recovered.

The first simultaneous RTMP/RIST run later failed its RTMP final-liveness
gate. The disposable FFmpeg RTMP listen muxer is single-client: after its TCP
writer blocked on a dead connection, its still-running supervisor appeared
healthy but could not accept OBS's reconnect. OBS consequently remained in
open/reconnect attempts for 532.2 seconds. RIST and the OBS process remained
healthy. This was a test-server availability failure, not accepted as a plugin
pass.

The RTMP lab was corrected in two ways:

- the FFmpeg server now has a ten-second output I/O timeout so its supervisor
  can normally re-arm a dead client; and
- the hostile matrix explicitly restarts the one-shot listener after the
  total blackout before measuring the final recovery window.

The corrected final-DLL matrix passed:

- 901 observations covered the entire 900-second window;
- two decoder epochs reset during intentional transport disconnects;
- 125 samples observed reconnect/inactive/opening state;
- the longest no-audio/no-video interval was 115.3 seconds during the extreme
  stage and disposable-listener recovery;
- the plugin reconnected after 19 attempts when the listener was re-armed,
  then again after two attempts following the final blackout;
- all final 60-second observations were live, advancing 2,768 audio and 1,775
  video frames;
- final counters were 9,464 audio and 6,110 video;
- final `avOffsetMs` was −275 ms (observed range −806 to +19,331 ms); and
- the strict final three-second liveness check passed.

During that corrected run, one OBS PID remained alive for all 190 process
samples over 945.6 seconds. Non-overlapping tail medians changed as follows:

| Resource | First tail | Last tail | Change |
|---|---:|---:|---:|
| Private bytes | 223.2 MiB | 230.5 MiB | +7.3 MiB |
| Handles | 1,719 | 1,721 | +2 |
| Threads | 36 | 37 | +1 |

Private bytes remained between 174.1 and 259.0 MiB and their fitted slope was
approximately −0.2 MiB/hour. Final shutdown completed in 596 ms, the OBS log
reported zero memory leaks, Windows recorded no application crash/hang event,
and WER produced no crash dump.

## Shutdown-test correction

Two earlier apparent RIST shutdown hangs were invalid measurements. The close
helper ran from SSH session 0 while OBS ran on the interactive desktop in
session 2, so `EnumWindows` saw zero OBS windows and never sent `WM_CLOSE`.
WinDbg showed OBS's main thread in its normal Qt event loop and its media
thread still reading; teardown had not begun.

The direct helper now fails when it sees zero windows. The SSH-safe
`measure-obs-shutdown.ps1` creates a temporary interactive scheduled task,
records both session IDs, signals the correct desktop, waits for process exit,
and removes the task. All shutdown times above use this corrected method.

## Persistent clean soak

The first dual RTMP/RIST clean-soak attempt began at
2026-07-30 08:55:37 UTC. It is retained as an invalidated harness run. At
4 hours 35 minutes, a progress check copied the two active JSONL files
directly with Windows OpenSSH `scp`. The reader held each file exclusively
while the corresponding observer tried to reopen it with `Add-Content`, so
both observer tasks stopped with a sharing-violation exception. All recorded
source samples through that point were active and playing with no reconnect
or counter reset. The OBS process and sources continued running: at
13.15 hours the PID was unchanged, fresh status counters advanced, tail
handles and threads had not grown, the 12-hour private-memory slope was
0.82 MiB/hour, and there was no matching crash event or dump. Those facts are
useful evidence, but the attempt is not counted as a 24-hour pass.

The checkpoint and process writers now keep shared, auto-flushed handles open
for the run. `snapshot-soak-results.ps1` copies live evidence through
`FileShare.ReadWrite`; only those snapshots are downloaded. A live writer
completed all 100 records during concurrent access, a mid-write snapshot
captured 11 valid records without delaying the writer, and a real dual
RTMP/RIST smoke completed both observers successfully with zero reconnects or
counter resets.

The replacement clean soak began at 2026-07-30 22:20:51 UTC on the same final
DLL and existing OBS PID. Its three isolated scheduled tasks use a new task
prefix and result directory. The planned source duration is 86,400 seconds,
the process monitor runs 86,460 seconds, and expected completion is
2026-07-31 22:21:51 UTC. The acceptance gate allows no reconnect sample,
counter reset, stale final counter, process/PID loss, crash/hang event, dump,
or excessive tail growth. This document does not claim that result before the
replacement run completes and its snapshots are analyzed.

## Safety and evidence

- No OBS crash report, Windows application-crash event, or application-hang
  event was recorded.
- The production OBS DLL SHA-256 remained
  `fe2c2ec873c185cd5730d33d83a314b08d9b4915f449d4409dd60f64a5e349d1`.
- The final isolated portable DLL SHA-256 was
  `dd994ed17e77d8dea0c707060f37ecb1c45c1921f0796640a33350904cd14271`.
- Completed-run JSON/JSONL, process CSV, OBS logs, Windows event evidence,
  qdisc events, FFmpeg logs, shutdown results, and checksums are retained
  beneath `artifacts/windows-qualification`.
- The invalidated clean-soak tasks were removed after their evidence was
  preserved. The replacement tasks, isolated labs, local-only WebSocket
  automation, WER dump capture, and RIST firewall rule intentionally remain
  active until the replacement 24-hour evidence is collected. They are not
  represented as cleaned up yet.

## Remaining network coverage

This qualifies the software-decode SRT, RTMP, and RIST paths under synthetic
hostile WAN conditions. It does not replace:

- completion and analysis of the in-progress lock-safe 24-hour clean
  dual-transport rerun;
- hardware-decode repetitions for each transport;
- a matched flash/beep fixture captured from OBS to measure content A/V error;
- multi-hour hostile-network soak testing; or
- impairment tests against a persistent production ingest/relay rather than
  a supervised FFmpeg listener.
