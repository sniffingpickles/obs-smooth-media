# Reliability Audit

Audit date: 2026-07-30
Upstream baseline reviewed: `8d79dd1` (OBS 32.0.4 / FFmpeg 7.1 target)

## Scope

This pass reviewed the complete native plugin path:

- OBS source creation, updates, activation, media controls, and destruction;
- media-thread ownership, cancellation, and reconnect behavior;
- FFmpeg open, probe, packet, decode, hardware fallback, and EOF paths;
- decoded video/audio validation and OBS handoff;
- audio-ring allocation, accounting, pacing, and adaptive statistics;
- stream/wall clock measurement and timestamp generation;
- obs-websocket vendor requests;
- CMake, Windows packaging, and CI; and
- historical simulations and current documentation.

## Material findings resolved

| Area | Failure mode | Resolution |
|---|---|---|
| Teardown | Stop could race a shared decoder pointer and free it while another thread still used it. Blocking opens could also hold OBS shutdown until timeout. | The media thread exclusively owns the decoder. Stop sets an atomic abort observed by FFmpeg's interrupt callback, then joins. |
| Reconnect | A second reconnect worker could race source reset/settings/destruction and made lock/join ordering fragile. | Reconnect is a timestamp serviced by `video_tick`; only the media thread opens or reads the network. |
| Shared state | `volatile bool` was being treated as inter-thread synchronization, and status reads mixed fields from different moments. | Cross-thread flags use OBS atomic helpers; mutable state has explicit lifecycle, timing, controller, state, clock, or buffer locks; WebSocket status uses a consistent snapshot. |
| Settings | Decoder configuration strings could be released while the media thread was still constructing or logging its decoder. | Decoder-affecting updates stop and join before replacing strings. Allocation failure leaves the prior configuration intact. |
| Notifications | A connection completing during cancellation could queue a stale `media_started` notification after the source had stopped. | Cancellation is checked after open and notification flags are cleared after join. OBS media signals are emitted outside the lifecycle lock. |
| Audio ring | A popped frame was swapped into staging, but duration accounting used the cleared ring slot. Reported buffer depth therefore did not decrease. | Pop accounting now uses the staged frame, with a regression assertion for count and duration. |
| Audio memory | Pop could expose storage to concurrent reuse; allocation failure could partially mutate a slot; large cached slots could retain excessive memory. | Pop swaps into isolated staging storage, growth allocations commit atomically, and allocations above 64 KiB are released instead of retained in all 512 slots. |
| Buffer control | The old 128-slot ring could not prime the adaptive target for 2.5 ms frames. Trimming could undershoot target, and every empty tick inflated underrun credit. | The bounded ring has 512 slots, trim preserves at least target depth, and one starvation episode produces one underrun event. |
| Clock | The history silently shortened the five-second window for small frames; readers raced the writer; discontinuities polluted the rate estimate; extreme arithmetic could overflow. | History has 2,048 circular slots, access is mutex-protected, backwards epochs reset, and rate/drift math is bounded. |
| Decode | Per-read packet allocation, codec `EAGAIN`, libRIST read `EAGAIN`, unflushed delayed frames, and one loss-corrupted packet could lose media or tear down the whole transport. | One reusable packet is used; codec `EAGAIN` drains then retries the same packet; libRIST read `EAGAIN` is a live gap; EOF flushes; corrupt packet/frame errors are dropped and rate-limited while `ENOMEM` remains fatal. |
| Open/probe | Protocol timeout options are inconsistent, and a dead RIST listener could leave open/probe blocked through source or OBS shutdown. | FFmpeg's interrupt callback observes the external atomic abort and enforces a 35-second open/probe deadline independent of protocol options. |
| Initialization | Internal audio/clock mutex creation and hardware-device reference allocation were assumed to succeed. A null hardware pixel-format name was passed to `%s`. | Initialization now returns and unwinds on mutex failure, hardware setup checks `av_buffer_ref`, and logging has a safe unknown-format fallback. |
| Frame validation | Invalid formats, dimensions, planes, strides, channel counts, data sizes, side-data lengths, or timestamp arithmetic could reach unsafe OBS/FFmpeg paths. | Inputs are checked and bounded before output; malformed frames are dropped; timestamp additions saturate. |
| A/V timing | Audio timestamps used stream PTS while OBS consumed samples at a corrected declared rate; video used a different rate/delay model. | Audio advances by samples/declared rate; video uses the published playback ratio and actual/target audio depth. |
| WebSocket | `RestartSource` reported success after reapplying unchanged settings, which did not restart. Status performed racy direct reads. | Restart invokes the OBS media restart action; status uses the locked snapshot API. |
| Secrets | Full stream URLs, which commonly contain keys/tokens, were written to OBS logs. | Operational logs no longer include stream URLs. API responses retain documented URL behavior. |

## Verification evidence

The final source passed:

- native strict-warning build and all six CTest tests;
- AddressSanitizer + UndefinedBehaviorSanitizer and all six tests;
- ThreadSanitizer and all six tests;
- Clang static analysis with no diagnostics;
- strict syntax compilation of the complete plugin against OBS 32.0.4
  headers and current FFmpeg headers;
- strict x86-64 Windows-target compilation of every plugin translation unit;
- real FFmpeg decode with delayed B-frame EOF flush verification and a
  deliberately corrupt MPEG-TS interval followed by clean recovery GOPs;
- cancellation of a blocked UDP open in under three seconds;
- 10,000-frame concurrent producer/consumer/observer stress;
- 100 repeated normal core runs, 20 ASan/UBSan core+decoder runs, 10 TSan
  core+decoder runs, 20 repeated decoder/cancellation runs, and 500 repeated
  Windows native core runs; and
- deterministic 0.97–1.03×, 30/60 fps, 0–15 ms jitter scenarios. The worst
  settled matched-content A/V error was 32.7 ms against a 40 ms limit.

Commands and release qualification steps are in [testing.md](testing.md).

## Windows OBS qualification

The final source was also built and exercised on a real Windows host:

- Windows Server 2022 build 20348, Intel Core i7-7700, Intel HD Graphics 630
  driver 24.20.100.6290, and an active RDP desktop session;
- OBS Studio portable 32.0.4 with obs-websocket 5.6.3;
- FFmpeg 7.1 shared libraries; and
- MSVC 19.44 with the Windows 10.0.26100 SDK.

The Windows pass produced the following evidence:

- a clean Release DLL build against the pinned OBS/FFmpeg versions;
- 500 consecutive native concurrent core-test passes;
- 10 seconds of software playback plus 20 complete decoder restarts;
- D3D11 hardware decode on the Intel GPU plus 10 complete decoder restarts;
- 30 seconds of 44.1 kHz AAC playback plus five decoder restarts;
- 60 seconds of a 0.98×-paced H.264/AAC feed with no underrun, reconnect, or
  loss of playing state;
- a forced six-second SRT sender outage, transport timeout, one reconnect
  attempt, successful re-open, and resumed audio/video within a 75-second
  observation;
- all five vendor requests, including missing-source and missing-URL error
  contracts;
- shutdown with several active sources in 590 ms and shutdown during a
  blocked SRT reconnect in 665 ms;
- zero OBS-reported memory leaks on all three complete shutdown logs; and
- no OBS crash report, Windows crash event, or Windows hang event during the
  qualification.

The existing OBS installation, scenes, and v1.4.12 production DLL were not
modified. The tests used an isolated portable OBS tree and local synthetic
streams. Detailed JSON results and OBS/FFmpeg logs remain under
`C:\obs-smooth-audit-20260730` on the qualification host.

This pass also found and resolved four Windows-specific release defects:

- the dependency script used an obsolete OBS archive name and did not pin the
  OBS header checkout to 32.0.4;
- MSVC C11 atomics require `/experimental:c11atomics` in addition to C11 mode;
- POSIX `strdup` produced Windows portability warnings; and
- `SetStreamURL` accepted an omitted/empty URL despite the documented error
  contract.

## Hostile WAN qualification

A subsequent public-WAN pass placed a Linux `tc netem` impairment namespace
between the SRT sender and the isolated Windows OBS receiver. It exercised
bidirectional delay and jitter, 1–30% correlated loss, 25% reordering,
duplication, approximately 1.75–2.25 second RTT conditions, 384/512 Kbps
ceilings against a 4.1 Mbps feed, repeated abrupt path changes, and a
40-second blackout.

The 900-second hostile observer passed its reconnect/recovery contract. OBS
survived 28 disconnect/reconnect cycles, recovered about 1.9 seconds after the
final blackout cleared, played for the entire final 90-second clean window,
and shut down in 527 ms with zero reported leaks. Handles and threads ended
below their starting samples; process memory remained bounded.

A separate 300-second rate-only run isolated bandwidth collapse from loss and
latency. All 302 samples remained active and playing with no reconnect through
2 Mbps, 1 Mbps, and repeated 384 Kbps ceilings. Output necessarily slowed
during starvation, then returned to the expected rate in the clean recovery
windows while stale backlog was bounded.

The complete topology, stage definitions, quantitative results, limitations,
and cleanup evidence are in
[wan-qualification-2026-07-30.md](wan-qualification-2026-07-30.md).

RTMP and RIST were then exercised through independent namespaces while both
sources shared one OBS process:

- the full 900-second RIST hostile matrix remained active and `playing` for
  all 900 samples, including a 40-second blackout, without reconnect or
  counter reset;
- the final-DLL corrected RTMP matrix observed real disconnect/reconnect
  epochs, recovered after the final blackout, and passed 901 observations plus
  the final three-second live-counter gate;
- a simultaneous 903-second RIST clean control had no reconnect/reset and
  bounded A/V status between −344 and −248 ms;
- one OBS PID remained present for all 190 process samples; private-byte tail
  medians grew 7.3 MiB, handles grew two, and threads grew one;
- correct interactive-session shutdown completed in 596 ms with zero
  OBS-reported leaks; and
- there was no WER dump or Windows application crash/hang event.

The first simultaneous RTMP run is retained as a failed test, not hidden: its
one-shot FFmpeg listen muxer remained attached to a dead TCP client and could
not accept OBS's reconnect. The lab now applies a write timeout and
deterministically re-arms that disposable listener before final recovery is
judged.

Two apparent RIST shutdown hangs were also reclassified as harness false
positives. An SSH session cannot enumerate the OBS window in the interactive
desktop session, so the old helper sent no close signal. The replacement
records both session IDs and uses a temporary interactive scheduled task.

The first strict 24-hour attempt is retained as an invalidated harness run,
not a plugin failure. At 4 hours 35 minutes, downloading both live JSONL files
with Windows OpenSSH `scp` held exclusive read handles while the observers
tried to reopen the files with `Add-Content`. Both observers stopped with an
explicit sharing-violation exception. Their final samples were active and
playing with no reconnect or counter reset. OBS remained on the same PID, and
a later 13.15-hour process snapshot plus fresh WebSocket samples showed both
sources still playing and advancing with no crash event or dump.

The observer and process monitor now keep shared, auto-flushed writers open
for the run, and a dedicated helper copies live evidence through
`FileShare.ReadWrite` before download. A real concurrent-read smoke completed
both RTMP and RIST observers successfully. A fresh strict 24-hour rerun began
at 2026-07-30 22:20:51 UTC and is expected to finish at
2026-07-31 22:21:51 UTC. No pass is claimed until at least 86,300 seconds of
the replacement evidence is analyzed.

GitHub Actions passed on proposed commit `515bc47`: the Windows job built and
uploaded the OBS 32.0.4 / FFmpeg 7.1 x64 plugin, and the Linux job passed the
strict, ASan/UBSan, and TSan suites. The resulting CI DLL is an x86-64 Windows
PE module with SHA-256
`4f9b3d6f1dcff6b05cfe370917ce3ec6c238b18221ee669184ab46e9935e0566`.
At 1 hour 22 minutes, a non-blocking snapshot of the replacement soak also
passed analysis with zero reconnect samples, counter resets, or source stalls
over 1.04 seconds. Process tail medians were flat: private bytes decreased
30 KiB, handles were unchanged, and threads decreased by 0.5.

## Residual qualification

“Crash-proof” cannot be proven for a native plugin embedded in OBS and using
FFmpeg, GPU drivers, protocol libraries, and third-party filters. The isolated
controller/decoder evidence and the Windows OBS pass are strong, but a release
still needs:

- NVDEC and QSV coverage on representative NVIDIA and newer Intel GPUs;
- SRT hardware-decode and multi-hour hostile-network repetitions;
- matched-content A/V capture during bandwidth recovery;
- rapid show/hide, scene-switch, source-update, and OBS-shutdown loops;
- audio-only, video-only, stereo, 5.1, and 7.1 format coverage; and
- completion of the active lock-safe 24-hour Windows OBS rerun while
  monitoring PID, memory, handles, threads, liveness, A/V status, and
  shutdown latency.

Those tests cover host callbacks, drivers, and protocol implementations that
cannot be reproduced by the standalone suite.
