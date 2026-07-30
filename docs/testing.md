# Reliability Test Strategy

## Automated acceptance gates

Every change should pass:

1. strict-warning native compilation;
2. audio-buffer and clock unit tests;
3. concurrent producer/consumer/observer stress tests;
4. real FFmpeg audio/video decode through the production decoder;
5. recovery through deliberately corrupted encoded video followed by clean
   GOPs;
6. external cancellation and a hard deadline for blocked stream opens;
7. deterministic 0.97–1.03× timing/jitter scenarios;
8. AddressSanitizer + UndefinedBehaviorSanitizer; and
9. ThreadSanitizer.

The timing model requires monotonic audio/video timestamps, bounded buffering,
no avoidable stable-feed underrun, and less than 40 ms matched-content A/V
error after settling.

## Normal local run

```bash
cmake -S . -B build-test -G Ninja \
  -DBUILD_PLUGIN=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

Decoder integration tests are enabled when `pkg-config`, FFmpeg development
libraries, and the `ffmpeg` executable are available. Otherwise the core and
Python timing tests still run.

## Memory and undefined-behavior sanitizers

```bash
cmake -S . -B build-asan -G Ninja \
  -DBUILD_PLUGIN=OFF \
  -DBUILD_TESTING=ON \
  -DENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## Thread sanitizer

```bash
cmake -S . -B build-tsan -G Ninja \
  -DBUILD_PLUGIN=OFF \
  -DBUILD_TESTING=ON \
  -DENABLE_THREAD_SANITIZER=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

ASan/UBSan and TSan are separate builds because those runtimes cannot be
combined.

## Windows release qualification

The GitHub Windows workflow compiles against the pinned OBS 32.0.4 headers and
FFmpeg 7.1 shared libraries. Before a release, install that artifact into a
clean OBS portable directory and run:

- software and hardware decode;
- audio-only, video-only, mono, stereo, 5.1, and 7.1 inputs;
- 30/60 fps and 44.1/48 kHz;
- RTMP, SRT, and RIST;
- show/hide and activate/deactivate loops;
- repeated URL/options/hardware-decode updates;
- WebSocket `SetStreamURL`, `GetStatus`, `RestartSource`, and `ListSources`;
- OBS shutdown during DNS/connect/probe/read stalls; and
- at least a 24-hour steady-stream soak.

Pass criteria:

- no OBS crash, hang, sanitizer finding, or unbounded memory growth;
- shutdown/restart completes promptly while the remote endpoint is dead;
- no backwards timestamps;
- no repeating reconnect-thread growth (there is no reconnect worker);
- bounded audio buffer/drop counts;
- no avoidable underrun on a stable 0.97–1.03× feed; and
- matched-content A/V error stays below 40 ms after settling.

### Windows obs-websocket harness

[`tests/windows/obs-e2e.ps1`](../tests/windows/obs-e2e.ps1) creates or updates
an isolated Smooth Media Source through obs-websocket, checks the five vendor
requests and their error contracts, polls playback counters, and performs
restart or reconnect assertions.

Example against a local MPEG-TS fixture:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tests/windows/obs-e2e.ps1 `
  -SourceName "Smooth Media Audit" `
  -StreamUrl "udp://127.0.0.1:19001?fifo_size=1000000&overrun_nonfatal=1" `
  -StableSeconds 30 `
  -RestartIterations 20 `
  -ResultPath artifacts/udp-software-e2e.json
```

Use `-HardwareDecoding` for the GPU path. For an externally controlled outage,
use `-ExpectReconnectCycle` and keep the observation window longer than the
transport's loss timeout. Use `-AllowTransientDisconnects` when transitions
are permitted but not required and only the recovered final state should be
asserted. `-FinalStreamUrl` is intended for shutdown testing:
set it to a deliberately unreachable endpoint, then close OBS while
connect/probe/read is blocked.

When `-ResultPath` is set, the harness also writes a sibling `.jsonl`
checkpoint after every observation. A crash, forced stop, or lost SSH session
therefore preserves all completed samples instead of losing the entire run.
The final JSON records a failure summary even when an assertion throws. Every
successful run also requires audio and video counters to advance during a
short final liveness window; an old nonzero counter in a silently stalled
`playing` source cannot pass.

Run this only against an isolated OBS profile. If authentication is disabled
for automation, firewall the WebSocket port from non-loopback clients and
restore authentication immediately after the run.

## Bad-network matrix

Use [`sims/netsim`](../sims/netsim/README.md) with Verbose Debug Logging:

```bash
sims/netsim/run-all.sh srt
```

Repeat for supported transports and add packet loss/jitter with `tc netem` or
Clumsy as described in the netsim guide. Capture:

- OBS log;
- protocol and codec;
- hardware-decode setting;
- source settings;
- stream/server clock rate;
- `rate`, `decl_sr`, `buf`, `tgt`, `jit`, `drop`, and `under` diagnostics;
- time-to-first audio/video; and
- recovery time after each outage.

The Python simulations are executable specifications, not substitutes for
this OBS/driver/network soak.

### Isolated Linux WAN lab

For true bidirectional impairment without applying `netem` to SSH or unrelated
host traffic, use the network-namespace harness:

```bash
sudo tests/network/wan-netem.sh setup eth0
sudo tests/network/wan-netem.sh make-fixture
sudo tests/network/wan-netem.sh start-feed
sudo tests/network/run-hostile-matrix.sh
sudo tests/network/run-bitrate-cliffs.sh
sudo tests/network/wan-netem.sh cleanup
```

The generic script defaults to SRT. Use
[`wan-netem-rtmp.sh`](../tests/network/wan-netem-rtmp.sh) or
[`wan-netem-rist.sh`](../tests/network/wan-netem-rist.sh) for isolated TCP
RTMP or UDP RIST labs. RIST push mode requires the receiver host as the
`start-feed` argument. Each wrapper uses distinct namespace, veth, port, and
artifact paths, so RTMP and RIST can run concurrently.

Only the selected transport crosses the shaped veth pair. `cleanup` restores
the original IP-forwarding value and removes the namespace and firewall rules.
The FFmpeg feed is supervised because a one-shot listener/sender can exit when
its peer times out. Every scenario deletes both old qdiscs before adding the
new one so rate ceilings and other omitted options cannot bleed into the next
stage.

FFmpeg's single-client RTMP listen muxer can remain blocked on the old TCP
connection after a total blackout instead of accepting OBS's reconnect. This
is a limitation of the disposable test server, not production RTMP ingest
behavior. Run its hostile matrix with
`RESTART_FEED_AFTER_BLACKOUT=1`; the matrix explicitly restarts the listener
after returning the link to clean and before timing the final recovery window.
The RTMP lab also applies a ten-second output I/O timeout so its supervisor can
normally discard a dead client and re-arm without waiting for the final
explicit restart.

Use [`tests/windows/close-obs.ps1`](../tests/windows/close-obs.ps1) from the
same interactive Windows session as OBS when an automated graceful `WM_CLOSE`
shutdown measurement is required. It now fails if it cannot see any OBS
window; a zero-window result is not evidence of a shutdown hang.

From SSH or another Windows session, use
[`tests/windows/measure-obs-shutdown.ps1`](../tests/windows/measure-obs-shutdown.ps1).
It creates a temporary interactive scheduled task for the close signal,
measures process exit, records both session IDs, and removes the task. Use
[`tests/windows/monitor-obs-process.ps1`](../tests/windows/monitor-obs-process.ps1)
during long soaks to checkpoint working set, private bytes, handles, threads,
CPU time, PID continuity, and process liveness.

For a disconnect-safe dual RTMP/RIST soak, use
[`tests/windows/start-dual-soak.ps1`](../tests/windows/start-dual-soak.ps1).
It launches two strict liveness observers and the process monitor as
interactive scheduled tasks, so closing the SSH session does not stop the
test. Its default duration is 24 hours. Remove the three dedicated tasks after
collecting results with
[`tests/windows/remove-dual-soak-tasks.ps1`](../tests/windows/remove-dual-soak-tasks.ps1).

Evaluate the checkpoints with
[`tests/analyze-soak.py`](../tests/analyze-soak.py). For a clean 24-hour run,
require at least 86,300 seconds of evidence, constrain observation gaps, and
allow no reconnect samples or counter resets:

```bash
python3 tests/analyze-soak.py \
  --process-csv artifacts/dual-soak/obs-process.csv \
  --source-jsonl artifacts/dual-soak/rtmp-e2e.jsonl \
  --source-jsonl artifacts/dual-soak/rist-e2e.jsonl \
  --min-duration-seconds 86300 \
  --max-sample-gap-seconds 75 \
  --output artifacts/dual-soak/analysis.json
```

The analyzer compares non-overlapping tail medians instead of trusting one
start/end sample. It rejects process disappearance or PID changes,
non-monotonic/missing observations, excessive private-byte/handle/thread
growth, audio or video stalls, reconnect/inactive samples, counter resets, and
stale final counters.
