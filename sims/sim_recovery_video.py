#!/usr/bin/env python3
"""
sim_recovery_video.py

HISTORICAL MODEL: this script models an earlier implementation. It is retained
to reproduce the failure modes that led to the current recovery design, not to
validate current source. Use tests/test-timing-model.py for acceptance testing.

Faithful pure-math simulation of the `obs-smooth-media` OBS plugin, focused on:
  (1) the VIDEO timestamp path and A/V sync over long sessions, and
  (2) reconnect / stall / scene-switch recovery dynamics.

No OBS, no FFmpeg, no threads.  All timing logic is ported line-for-line from
the C source so the numbers reflect what the real plugin does:

  src/smooth-media-source.c
    - on_video_frame()            -> VideoTS.on_frame()
    - smooth_media_tick()  audio  -> AudioEngine.tick()
    - start_media / reconnect     -> RecoveryModel
  src/audio-buffer.c              -> JitterBuffer
  src/clock-tracker.c             -> ClockTracker
  src/stream-decoder.c
    - deliver_video_frame()       -> source frame PTS generation + dur fallback

All time is in integer nanoseconds, and integer divisions use C truncation
(toward zero) so drift-correction rounding matches the compiled plugin.
"""

import math
import random
from collections import deque

# ─────────────────────────────────────────────────────────────────────────────
#  Constants (mirrored from the C source)
# ─────────────────────────────────────────────────────────────────────────────
MS = 1_000_000
SEC = 1_000_000_000

JITTER_BUFFER_MS = 80                     # smooth-media-source.c
MIN_BUFFER_NS = JITTER_BUFFER_MS * MS     # audio_buf.min_buffer_ns == buf_offset
MAX_BUFFER_NS = 500 * MS
AUDIO_BUF_MAX_FRAMES = 128                # audio-buffer.h
SR_WARMUP_NS = 5 * SEC
SR_HOLD_TIME_NS = 3 * SEC
CLOCK_WINDOW_NS = 5 * SEC                 # clock-tracker.c DEFAULT_WINDOW_NS
EMA_ALPHA = 0.02

RENDER_FPS = 60
TICK_NS = SEC // RENDER_FPS               # 16,666,666 ns  (OBS video_tick cadence)

AUDIO_FRAMES = 1024
AUDIO_SR = 48000
ADUR_NS = AUDIO_FRAMES * SEC // AUDIO_SR  # 21,333,333 ns


def tdiv(a, b):
    """C int64 division: truncate toward zero (Python // floors, so emulate)."""
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


# ─────────────────────────────────────────────────────────────────────────────
#  stream-decoder.c : deliver_video_frame PTS generation
#  best_effort_timestamp -> pts_ns; duration fallback ~30fps; next_pts_ns.
# ─────────────────────────────────────────────────────────────────────────────
def gen_video_pts(n, fps, jitter_ns=0, reorder_prob=0.0, drop_pts_prob=0.0,
                  rng=None):
    """Yield (pts_ns, keyframe) like deliver_video_frame would hand to
    on_video_frame.  jitter_ns: +/- jitter on best_effort_timestamp.
    reorder_prob: probability that two adjacent display PTS get swapped
    (B-frame / reordering artifact).  drop_pts_prob: best_effort==NOPTS so
    the decoder falls back to next_pts_ns (monotone estimate)."""
    rng = rng or random.Random(1)
    base = SEC // fps  # nominal spacing
    fallback_dur = 33_333_333 if fps == 30 else base  # ~30fps fallback in C
    next_pts = 0
    out = []
    gop = max(1, int(round(fps * 2)))  # keyframe every ~2s
    for i in range(n):
        ideal = i * base
        if rng.random() < drop_pts_prob:
            pts = next_pts            # NOPTS -> ctx->next_pts_ns
        else:
            j = rng.randint(-jitter_ns, jitter_ns) if jitter_ns else 0
            pts = ideal + j
        next_pts = pts + (base if base > 0 else fallback_dur)
        out.append([pts, (i % gop) == 0])
    # simulate display-order reordering noise (non-monotonic deltas)
    for i in range(1, len(out)):
        if rng.random() < reorder_prob:
            out[i - 1][0], out[i][0] = out[i][0], out[i - 1][0]
    return out


# ─────────────────────────────────────────────────────────────────────────────
#  smooth-media-source.c : on_video_frame()  (VIDEO is timing master)
# ─────────────────────────────────────────────────────────────────────────────
class VideoTS:
    def __init__(self, buf_offset_ns=MIN_BUFFER_NS):
        self.buf_offset = buf_offset_ns
        self.reset()

    def reset(self):
        self.video_frames_out = 0
        self.prev_video_pts = 0
        self.video_next_ts = 0
        self.video_out_ts = 0
        self.first_video = False
        self.got_first_keyframe = False
        self.snap_count = 0          # times we fell to the wall fallback
        self.clamp_count = 0         # times max_ahead clamp fired

    def on_frame(self, pts_ns, wall_now, keyframe):
        # Skip until first keyframe
        if not self.got_first_keyframe:
            if not keyframe:
                return None
            self.got_first_keyframe = True

        if not self.first_video:
            self.first_video = True

        buf_offset = self.buf_offset

        if self.video_frames_out == 0:
            out_ts = wall_now + buf_offset
            self.video_next_ts = wall_now + buf_offset
            self.prev_video_pts = pts_ns
        else:
            pts_delta = pts_ns - self.prev_video_pts
            self.prev_video_pts = pts_ns

            if 0 < pts_delta < 500 * MS:
                self.video_next_ts += pts_delta
            else:
                self.video_next_ts = wall_now + buf_offset
                self.snap_count += 1

            drift_target = wall_now + buf_offset
            drift_error = drift_target - self.video_next_ts
            if drift_error < 0:
                self.video_next_ts += tdiv(drift_error, 10)     # ahead: 10%
            elif drift_error > 100 * MS:
                self.video_next_ts += tdiv(drift_error, 100)    # behind: 1%
            else:
                self.video_next_ts += tdiv(drift_error, 1000)   # steady: 0.1%

            max_ahead = drift_target + 20 * MS
            if self.video_next_ts > max_ahead:
                self.video_next_ts = max_ahead
                self.clamp_count += 1

            out_ts = self.video_next_ts

        self.video_out_ts = out_ts
        self.video_frames_out += 1
        return out_ts


# ─────────────────────────────────────────────────────────────────────────────
#  audio-buffer.c : jitter buffer
# ─────────────────────────────────────────────────────────────────────────────
class JitterBuffer:
    def __init__(self):
        self.frames = deque()          # (pts_ns, nframes, sample_rate)
        self.total_buffered_ns = 0
        self.primed = False
        self.frames_dropped = 0
        self.min_buffer_ns = MIN_BUFFER_NS
        self.max_buffer_ns = MAX_BUFFER_NS

    @staticmethod
    def _fd(f):
        return f[1] * SEC // f[2]

    def reset(self):
        self.frames.clear()
        self.total_buffered_ns = 0
        self.primed = False
        # frames_dropped persists across reset in C? No: audio_buffer_reset
        # frees+reinits, zeroing stats. Match that.
        self.frames_dropped = 0

    def push(self, pts, nframes, sr):
        if len(self.frames) >= AUDIO_BUF_MAX_FRAMES:
            old = self.frames.popleft()
            self.total_buffered_ns -= self._fd(old)
            self.frames_dropped += 1
        while self.frames and self.total_buffered_ns > self.max_buffer_ns:
            old = self.frames.popleft()
            self.total_buffered_ns -= self._fd(old)
            self.frames_dropped += 1
        self.frames.append((pts, nframes, sr))
        self.total_buffered_ns += nframes * SEC // sr
        if not self.primed and self.total_buffered_ns >= self.min_buffer_ns:
            self.primed = True

    def pop(self):
        if not self.frames or not self.primed:
            return None
        f = self.frames.popleft()
        self.total_buffered_ns -= self._fd(f)
        if self.total_buffered_ns < 0:
            self.total_buffered_ns = 0
        return f

    def level_ns(self):
        return self.total_buffered_ns


# ─────────────────────────────────────────────────────────────────────────────
#  clock-tracker.c
# ─────────────────────────────────────────────────────────────────────────────
class ClockTracker:
    def __init__(self):
        self.reset()

    def reset(self):
        self.history = deque()     # (stream_pts, wall)
        self.stream_rate = 1.0
        self.smoothed_rate = 1.0
        self.anchor_set = False

    def record(self, stream_pts_ns, wall_ns):
        if not self.anchor_set:
            self.anchor_set = True
        self.history.append((stream_pts_ns, wall_ns))
        # drop samples older than the window from the front for efficiency
        while self.history and (wall_ns - self.history[0][1]) > CLOCK_WINDOW_NS:
            # keep one just-outside sample like the C ring would, but the C
            # code only *considers* in-window samples, so trimming is fine.
            self.history.popleft()
        # find oldest in-window sample (front is oldest)
        if self.history:
            o_pts, o_wall = self.history[0]
            wall_elapsed = wall_ns - o_wall
            stream_elapsed = stream_pts_ns - o_pts
            if wall_elapsed > 100 * MS:
                r = stream_elapsed / wall_elapsed
                r = max(0.90, min(1.10, r))
                self.stream_rate = r
                self.smoothed_rate = (EMA_ALPHA * r +
                                      (1.0 - EMA_ALPHA) * self.smoothed_rate)

    def get_smoothed_rate(self):
        return self.smoothed_rate


# ─────────────────────────────────────────────────────────────────────────────
#  smooth-media-source.c : smooth_media_tick() audio drain + timestamp path
#  (sync_pts OFF is the default and what NOALBS/IRL use)
# ─────────────────────────────────────────────────────────────────────────────
class AudioEngine:
    def __init__(self, clock, sync_pts=False):
        self.clock = clock
        self.sync_pts = sync_pts
        self.reset()

    def reset(self):
        self.audio_frames_out = 0
        self.prev_audio_pts = 0
        self.audio_next_ts = 0
        self.audio_out_ts = 0
        self.last_audio_pop_time = 0
        self.audio_frame_dur_ns = 0
        self.first_audio = False
        self.stream_start_time = 0
        self.sr_hold_start = 0
        self.behind_clamp_count = 0
        self.underrun_ticks = 0

    def tick(self, wall_now, jb, video_frames_out):
        outs = []
        if not self.first_audio:
            return outs
        frame_dur = self.audio_frame_dur_ns or 21_333_333
        popped_any = False
        pops = 0
        while pops < 3:
            if self.last_audio_pop_time == 0:
                should_pop = jb.primed
            else:
                should_pop = (wall_now - self.last_audio_pop_time) >= frame_dur
            if not should_pop:
                break
            f = jb.pop()
            if f is None:
                break
            popped_any = True
            pts, nframes, sr = f
            if sr > 0 and nframes > 0:
                self.audio_frame_dur_ns = nframes * SEC // sr

            if self.last_audio_pop_time == 0:
                self.last_audio_pop_time = wall_now
            else:
                self.last_audio_pop_time += frame_dur
                if wall_now - self.last_audio_pop_time > frame_dur * 2:
                    self.last_audio_pop_time = wall_now - frame_dur

            rate = self.clock.get_smoothed_rate()  # affects adj_sr only

            # ── timestamp (sync_pts OFF default path) ──
            if self.sync_pts and video_frames_out > 0:
                out_ts = self._ts_syncpts(pts, wall_now)
            elif self.audio_frames_out == 0:
                out_ts = wall_now
                self.audio_next_ts = wall_now
                self.prev_audio_pts = pts
            else:
                pts_delta = pts - self.prev_audio_pts
                self.prev_audio_pts = pts
                if 0 < pts_delta < 500 * MS:
                    self.audio_next_ts += pts_delta
                else:
                    self.audio_next_ts = wall_now
                de = wall_now - self.audio_next_ts
                if de < 0:
                    self.audio_next_ts += tdiv(de, 10)
                elif de > 100 * MS:
                    self.audio_next_ts += tdiv(de, 100)
                else:
                    self.audio_next_ts += tdiv(de, 1000)
                max_ahead = wall_now + 20 * MS
                if self.audio_next_ts > max_ahead:
                    self.audio_next_ts = max_ahead
                if self.audio_next_ts < wall_now - 200 * MS:
                    self.audio_next_ts = wall_now
                    self.behind_clamp_count += 1
                out_ts = self.audio_next_ts

            self.audio_out_ts = out_ts
            self.audio_frames_out += 1
            pops += 1
            outs.append((out_ts, pts))

        if not popped_any:
            self.underrun_ticks += 1
        return outs

    def _ts_syncpts(self, pts, wall_now):
        if self.audio_frames_out == 0:
            self.audio_next_ts = wall_now
            self.prev_audio_pts = pts
            return wall_now
        pts_delta = pts - self.prev_audio_pts
        self.prev_audio_pts = pts
        if 0 < pts_delta < 500 * MS:
            self.audio_next_ts += pts_delta
        else:
            self.audio_next_ts = wall_now
        de = wall_now - self.audio_next_ts
        if de < 0:
            self.audio_next_ts += tdiv(de, 10)
        else:
            self.audio_next_ts += tdiv(de, 100)
        if self.audio_next_ts > wall_now:
            self.audio_next_ts = wall_now
        if self.audio_next_ts < wall_now - 200 * MS:
            self.audio_next_ts = wall_now
            self.behind_clamp_count += 1
        return self.audio_next_ts


# ─────────────────────────────────────────────────────────────────────────────
#  Event-driven session: feed arrivals into the real timestamp engines.
# ─────────────────────────────────────────────────────────────────────────────
def run_session(duration_s, rate, fps, net_jitter_ms=0.0, base_latency_ms=50.0,
                pts_jitter_ns=0, reorder_prob=0.0, drop_pts_prob=0.0,
                seed=1, sync_pts=False):
    """Run a clean session.  rate = stream_pts_advance / wall_advance.
    Returns dict of recorded video/audio outputs and engine stats."""
    rng = random.Random(seed)
    base = SEC // fps
    n_video = int(duration_s * fps) + 5
    n_audio = int(duration_s * AUDIO_SR / AUDIO_FRAMES) + 5
    base_lat = int(base_latency_ms * MS)

    vpts = gen_video_pts(n_video, fps, pts_jitter_ns, reorder_prob,
                         drop_pts_prob, rng)

    # arrival_wall = pts/rate + base_latency + net_jitter
    def arr(pts):
        j = int(rng.gauss(0, net_jitter_ms * MS)) if net_jitter_ms else 0
        return int(pts / rate) + base_lat + j

    events = []  # (wall, kind, payload)
    for pts, kf in vpts:
        events.append((arr(pts), 0, (pts, kf)))         # 0 = video
    for j in range(n_audio):
        apts = j * ADUR_NS
        events.append((arr(apts), 1, apts))             # 1 = audio
    # tick events at 60Hz across the whole wall window
    wall_end = int(duration_s * SEC) + base_lat + 2 * SEC
    t = 0
    while t < wall_end:
        events.append((t, 2, None))                      # 2 = tick
        t += TICK_NS

    events.sort(key=lambda e: (e[0], e[1]))

    clock = ClockTracker()
    video = VideoTS()
    audio = AudioEngine(clock, sync_pts=sync_pts)
    jb = JitterBuffer()

    vid_out = []   # (content_pts, out_ts, wall)
    aud_out = []   # (content_pts, out_ts, wall)

    for wall, kind, payload in events:
        if kind == 0:
            pts, kf = payload
            ts = video.on_frame(pts, wall, kf)
            if ts is not None:
                vid_out.append((pts, ts, wall))
        elif kind == 1:
            apts = payload
            clock.record(apts, wall)
            if not audio.first_audio:
                audio.first_audio = True
            jb.push(apts, AUDIO_FRAMES, AUDIO_SR)
        else:
            for out_ts, apts in audio.tick(wall, jb, video.video_frames_out):
                aud_out.append((apts, out_ts, wall))

    return dict(vid=vid_out, aud=aud_out, video=video, audio=audio, jb=jb,
                clock=clock)


def interp_ts(samples, query_pts):
    """samples sorted list of (content_pts, out_ts).  Linear-interp out_ts at
    query content pts.  Returns None if out of range."""
    # samples may not be perfectly sorted by pts (reordering); sort.
    if not samples:
        return None
    lo, hi = samples[0][0], samples[-1][0]
    if query_pts < lo or query_pts > hi:
        return None
    # binary search
    import bisect
    keys = [s[0] for s in samples]
    i = bisect.bisect_left(keys, query_pts)
    if i < len(keys) and keys[i] == query_pts:
        return samples[i][1]
    if i == 0:
        return samples[0][1]
    if i >= len(samples):
        return samples[-1][1]
    p0, t0 = samples[i - 1]
    p1, t1 = samples[i]
    if p1 == p0:
        return t0
    return t0 + (t1 - t0) * (query_pts - p0) / (p1 - p0)


def lipsync_timeline(res):
    """True lip-sync error = ts_video(content p) - ts_audio(content p),
    matched by shared stream PTS.  Returns list of (wall_s, err_ms)."""
    aud_sorted = sorted(res['aud'], key=lambda x: x[0])
    aud_pp = [(p, t) for (p, t, w) in aud_sorted]
    tl = []
    for (pts, ts_v, wall) in res['vid']:
        ts_a = interp_ts(aud_pp, pts)
        if ts_a is None:
            continue
        tl.append((wall / SEC, (ts_v - ts_a) / MS))
    return tl


def summ(vals):
    if not vals:
        return (0, 0, 0, 0)
    return (min(vals), max(vals), sum(vals) / len(vals), vals[-1])


# ─────────────────────────────────────────────────────────────────────────────
#  SCENARIO 1 — Long-session A/V drift
# ─────────────────────────────────────────────────────────────────────────────
def scenario1():
    print("=" * 78)
    print("SCENARIO 1 — Long-session A/V drift (300 s)")
    print("=" * 78)
    for rate in (1.0, 0.98):
        for fps in (30, 60):
            res = run_session(300, rate, fps, net_jitter_ms=3.0)
            tl = lipsync_timeline(res)
            errs = [e for (_, e) in tl]
            mn, mx, avg, last = summ(errs)
            # diag-style instantaneous out_ts gap (audio_out - video_out)
            gap_ms = (res['audio'].audio_out_ts - res['video'].video_out_ts) / MS
            # drift rate via linear fit ms/min over the timeline
            drift = 0.0
            if len(tl) > 100:
                t0, e0 = tl[10]
                t1, e1 = tl[-1]
                if t1 > t0:
                    drift = (e1 - e0) / (t1 - t0) * 60.0
            jb = res['jb']
            au = res['audio']
            print(f"\n rate={rate:.2f} fps={fps}:")
            print(f"   true lip-sync err (ms): min={mn:+.1f} max={mx:+.1f} "
                  f"avg={avg:+.1f} final={last:+.1f}")
            print(f"   |err| bound: {max(abs(mn), abs(mx)):.1f} ms "
                  f"(target <40 ms) -> "
                  f"{'OK' if max(abs(mn), abs(mx)) < 40 else 'FAIL'}")
            print(f"   drift rate: {drift:+.2f} ms/min")
            print(f"   diag av_wall (audio_out-video_out) = {gap_ms:+.1f} ms "
                  f"(expected ~-80, video offset fwd)")
            print(f"   audio underrun ticks={au.underrun_ticks}  "
                  f"jb dropped={jb.frames_dropped}  "
                  f"behind-clamps={au.behind_clamp_count}")
            print(f"   smoothed clock rate={res['clock'].get_smoothed_rate():.4f}")


# ─────────────────────────────────────────────────────────────────────────────
#  SCENARIO 2 — Video burst (GPU contention / decode catch-up)
# ─────────────────────────────────────────────────────────────────────────────
def scenario2():
    print("\n" + "=" * 78)
    print("SCENARIO 2 — Video burst / decode catch-up clamp")
    print("=" * 78)
    for fps in (30, 60):
        base = SEC // fps
        video = VideoTS()
        wall = 0
        out = []
        # 3s of normal frames
        n_norm = fps * 3
        for i in range(n_norm):
            pts = i * base
            wall = int(pts) + 50 * MS
            ts = video.on_frame(pts, wall, (i % (fps * 2)) == 0)
            if ts is not None:
                out.append((wall, ts))
        # STALL: 1s gap with no frames, wall jumps forward
        stall = 1 * SEC
        wall += stall
        # BURST: 1s of video (fps frames) all decoded within 5ms (catch-up)
        burst_pts0 = n_norm * base
        burst_wall0 = wall
        burst_ts = []
        for k in range(fps):
            pts = burst_pts0 + k * base
            w = burst_wall0 + int(k * (5 * MS) / fps)  # all within 5ms
            ts = video.on_frame(pts, w, False)
            if ts is not None:
                burst_ts.append(ts)
                out.append((w, ts))
        # output ts spacing during burst
        deltas = [(burst_ts[i] - burst_ts[i - 1]) / MS
                  for i in range(1, len(burst_ts))]
        mn, mx, avg, _ = summ(deltas)
        ahead = [(ts - (burst_wall0)) / MS for ts in burst_ts]
        print(f"\n fps={fps}: {fps} frames decoded in 5 ms after a 1 s stall")
        print(f"   output-ts spacing during burst (ms): "
              f"min={mn:.2f} max={mx:.2f} avg={avg:.2f}  "
              f"(ideal frame interval={1000/fps:.2f} ms)")
        print(f"   max_ahead clamps fired={video.clamp_count}  "
              f"snap-to-wall={video.snap_count}")
        print(f"   first burst frame ts is {ahead[0]:+.1f} ms vs burst wall; "
              f"last is {ahead[-1]:+.1f} ms")
        print(f"   -> timestamps cannot run more than "
              f"{(MIN_BUFFER_NS + 20*MS)/MS:.0f} ms ahead of wall "
              f"(clamp), so NO fast-forward; burst frames collapse onto the "
              f"clamp (≈0 ms apart => momentary judder, not speed-up).")


# ─────────────────────────────────────────────────────────────────────────────
#  SCENARIO 3 — Variable frame timing / B-frames / PTS reordering
# ─────────────────────────────────────────────────────────────────────────────
def scenario3():
    print("\n" + "=" * 78)
    print("SCENARIO 3 — Jittery PTS / B-frame reordering / NOPTS fallback")
    print("=" * 78)
    cases = [
        ("clean", 0, 0.0, 0.0),
        ("pts jitter +/-4ms", 4 * MS, 0.0, 0.0),
        ("B-frame reorder 8%", 2 * MS, 0.08, 0.0),
        ("NOPTS 10% + jitter", 4 * MS, 0.0, 0.10),
        ("heavy reorder 20%", 3 * MS, 0.20, 0.05),
    ]
    fps = 30
    for name, jit, reord, drop in cases:
        res = run_session(60, 1.0, fps, net_jitter_ms=2.0,
                          pts_jitter_ns=jit, reorder_prob=reord,
                          drop_pts_prob=drop)
        v = res['video']
        snap_pct = 100.0 * v.snap_count / max(1, v.video_frames_out)
        # output ts monotonicity / spacing
        ts = [t for (_, t, _) in res['vid']]
        back = sum(1 for i in range(1, len(ts)) if ts[i] < ts[i - 1])
        deltas = [(ts[i] - ts[i - 1]) / MS for i in range(1, len(ts))]
        mn, mx, avg, _ = summ(deltas)
        print(f"\n {name}:")
        print(f"   frames_out={v.video_frames_out}  snap-to-wall="
              f"{v.snap_count} ({snap_pct:.1f}%)  max_ahead clamps="
              f"{v.clamp_count}")
        print(f"   OUTPUT ts: backwards steps={back}  "
              f"spacing ms min={mn:+.2f} max={mx:+.2f} avg={avg:+.2f}")


# ─────────────────────────────────────────────────────────────────────────────
#  SCENARIO 4 — Stall then reconnect
# ─────────────────────────────────────────────────────────────────────────────
def scenario4():
    print("\n" + "=" * 78)
    print("SCENARIO 4 — Stall -> EOF -> reconnect recovery")
    print("=" * 78)
    # Recovery-timing model parameters (seconds)
    t_detect = TICK_NS / SEC          # tick latency to notice ENDED
    t_open_ok = 0.30                  # avformat_open_input + find_stream_info
    t_open_fail = 0.10                # fast refuse while server still down
    gop_s = 2.0                       # HEVC keyframe interval (IRL typical)
    kf_expected = gop_s / 2.0
    kf_worst = gop_s
    prime_s = JITTER_BUFFER_MS / 1000.0

    print(f"\n model: detect={t_detect*1000:.0f}ms open_ok={t_open_ok*1000:.0f}ms "
          f"GOP={gop_s:.0f}s prime={prime_s*1000:.0f}ms")
    print(f" {'stall':>6} {'delay':>6} {'1st-attempt':>12} {'recovery(exp)':>14} "
          f"{'recovery(worst)':>16} {'black/silent':>13}")
    for delay in (2, 10):
        for stall in (0.5, 1, 2, 4, 8):
            # reconnect_thread waits `delay` then start_media.  If the server
            # is still down (wall < stall), open fails fast and tick reschedules
            # after another `delay`.  First *successful* open is at the first
            # grid point >= stall.
            t = t_detect
            attempts = 0
            # first attempt fires at t_detect + delay (thread timedwait)
            t = t_detect + delay
            attempts = 1
            while t < stall:           # server still unreachable -> fail fast
                t += t_open_fail       # failed open returns
                # media thread -> ENDED, tick reschedules after delay
                t += t_detect + delay
                attempts += 1
            # now server reachable: successful open + keyframe + prime
            rec_exp = t + t_open_ok + kf_expected + prime_s
            rec_worst = t + t_open_ok + kf_worst + prime_s
            black = rec_exp           # screen black from t=0 (stall) to 1st frame
            print(f" {stall:>5.1f}s {delay:>5d}s {t:>11.2f}s "
                  f"{rec_exp:>13.2f}s {rec_worst:>15.2f}s "
                  f"{black:>12.2f}s  (attempts={attempts})")
    print("\n  Timestamp continuity across reconnect:")
    # After reset, video_frames_out=0 -> first out_ts = wall_now+80ms (fresh
    # wall anchor), audio_frames_out=0 -> out_ts = wall_now.  Both are >0 and
    # forward of any previous OBS timestamp because wall_now only increases.
    res_a = run_session(5, 1.0, 30)
    last_v = res_a['vid'][-1][1]
    video2 = VideoTS()
    new_wall = res_a['vid'][-1][2] + 12 * SEC   # reconnect 12s later
    new_ts = video2.on_frame(0, new_wall, True)
    print(f"   pre-reconnect last video out_ts = {last_v/SEC:.3f}s")
    print(f"   post-reconnect first video out_ts = {new_ts/SEC:.3f}s "
          f"(= new wall + 80ms)")
    print(f"   backwards/negative into OBS? "
          f"{'YES' if new_ts <= last_v else 'NO'} "
          f"(fresh wall anchor keeps ts strictly increasing)")


# ─────────────────────────────────────────────────────────────────────────────
#  SCENARIO 5 — Rapid scene switching (close_when_inactive=ON, NOALBS)
# ─────────────────────────────────────────────────────────────────────────────
def scenario5():
    print("\n" + "=" * 78)
    print("SCENARIO 5 — Rapid scene switching (close_when_inactive=ON)")
    print("=" * 78)
    t_join = 0.005       # stop_media_thread join (already-stopped -> instant)
    t_open_ok = 0.30     # SRT connect + find_stream_info
    gop_s = 2.0
    prime_s = JITTER_BUFFER_MS / 1000.0

    print(f"\n per SHOW -> start_media(): join={t_join*1000:.0f}ms "
          f"open={t_open_ok*1000:.0f}ms GOP={gop_s:.0f}s prime={prime_s*1000:.0f}ms")
    ttff_exp = t_join + t_open_ok + gop_s / 2.0
    ttff_worst = t_join + t_open_ok + gop_s
    ttff_best = t_join + t_open_ok + 0.0
    ttfa = t_join + t_open_ok + prime_s
    print(f"   time-to-first-FRAME : best={ttff_best*1000:.0f}ms "
          f"exp={ttff_exp*1000:.0f}ms worst={ttff_worst*1000:.0f}ms "
          f"(gated by next keyframe, 0..GOP)")
    print(f"   time-to-first-AUDIO : {ttfa*1000:.0f}ms "
          f"(open + 80ms jitter prime)")
    print("\n   Rapid toggle every 1-5s: each SHOW pays the full open+keyframe")
    print("   cost again (close_when_inactive tears the connection down on")
    print("   HIDE).  With a 2s GOP, time-to-first-frame can be as long as the")
    print("   on-screen dwell time itself.")

    # ---- threading race / deadlock analysis ----
    print("\n   THREADING RACE / DEADLOCK ANALYSIS")
    print("   " + "-" * 60)
    print("""   smooth_media_hide()/deactivate()/destroy()/update() call
   stop_reconnect_thread():
       lock(reconnect_mutex)
       if (reconnect_thread_valid) {
           signal(stop_event); pthread_join(reconnect_thread);  <-- under lock
       }
       unlock(reconnect_mutex)

   reconnect_thread_func():
       timedwait(stop_event, delay)        # signal only helps HERE
       start_media(s)                      # <-- long: stop_media_thread join +
                                           #     avformat_open_input (up to 30s)
       lock(reconnect_mutex)               # <-- tries to take the SAME lock
       reconnecting = false
       unlock(reconnect_mutex)

   DEADLOCK: if HIDE arrives while reconnect_thread_func is *past* the
   timedwait and inside start_media(), the stop_event signal is a no-op.
   stop_reconnect_thread() holds reconnect_mutex and blocks in pthread_join
   waiting for the reconnect thread to exit; the reconnect thread finishes
   start_media() and then blocks on lock(reconnect_mutex) which the joiner
   still holds.  Neither can proceed -> the OBS thread that called hide()
   (the UI / graphics thread) is frozen.""")
    # Quantify the vulnerable window per reconnect cycle.
    window = t_open_ok  # plus stop_media_thread join; open dominates
    cycle_min = 2.0     # reconnect_delay
    p_hit = window / (cycle_min + window)
    print(f"\n   Vulnerable window per reconnect cycle ≈ start_media() runtime")
    print(f"     ≈ {window*1000:.0f} ms (open) up to 30000 ms (open timeout on")
    print(f"     a dead host).  With delay=2s a single random HIDE during an")
    print(f"     active reconnect has ≈{p_hit*100:.0f}% chance of landing in the")
    print(f"     window when open is fast — and ≈94% when open hits the 30s")
    print(f"     timeout (30/(2+30)).  This is exactly the NOALBS hot path.")


# ─────────────────────────────────────────────────────────────────────────────
#  SCENARIO 6 — Reconnect storm
# ─────────────────────────────────────────────────────────────────────────────
def scenario6():
    print("\n" + "=" * 78)
    print("SCENARIO 6 — Reconnect storm (server down 60 s, delay=2 s)")
    print("=" * 78)
    down_s = 60.0
    delay = 2.0
    t_detect = TICK_NS / SEC
    for open_fail_s, label in ((0.05, "fast refuse (0.05s)"),
                               (30.0, "open timeout (30s)")):
        t = 0.0
        attempts = 0
        logs = 0
        threads_created = 0
        peak_live_threads = 1  # at most one media + one reconnect alternating
        while t < down_s:
            # tick notices ENDED, schedules reconnect
            t += t_detect
            # reconnect thread: wait delay, then attempt
            t += delay
            attempts += 1
            threads_created += 2   # reconnect_thread + media_thread, both joined
            if attempts == 1 or attempts % 10 == 0:
                logs += 1
            # start_media -> open fails after open_fail_s
            t += open_fail_s
        print(f"\n {label}:")
        print(f"   attempts in {down_s:.0f}s = {attempts}  "
              f"(cycle≈{delay+open_fail_s+t_detect:.2f}s)")
        print(f"   throttled log lines (#1 + every 10th) = {logs}")
        print(f"   threads created = {threads_created} (all pthread_join'd "
              f"serially) -> peak live = {peak_live_threads} reconnect + "
              f"1 media; NO unbounded growth, NO leak")
        print(f"   reconnect_attempts counter -> {attempts} (uint, bounded)")
    print("\n   Note: with the 30s open timeout, the reconnect thread spends")
    print("   nearly the whole cycle inside start_media() -> the Scenario-5")
    print("   deadlock window is ~94% of the time during a storm.")


# ─────────────────────────────────────────────────────────────────────────────
def main():
    random.seed(1)
    scenario1()
    scenario2()
    scenario3()
    scenario4()
    scenario5()
    scenario6()
    print("\n" + "=" * 78)
    print("DONE — historical recovery scenarios completed.")
    print("=" * 78)


if __name__ == "__main__":
    main()
