#!/usr/bin/env python3
"""
Faithful numerical simulation of the obs-smooth-media AUDIO output path.

HISTORICAL MODEL: this script models an earlier implementation. It is retained
to reproduce the failure modes that led to the current controller, not to
validate current source. Use tests/test-timing-model.py for acceptance testing.

This is a pure-math port (no OBS / FFmpeg) of:
  - src/clock-tracker.c   (rate estimation)
  - src/audio-buffer.c    (jitter buffer)
  - src/smooth-media-source.c :: smooth_media_tick (audio drain loop),
                                 on_audio_frame (push)

The goal is to drive the exact integer/float logic with adversarial IRL
network conditions and measure empirical failure modes (audible glitches,
pitch steps, underruns, latency, recovery time).

Everything below mirrors the C as literally as practical. All times are in
nanoseconds (int64) exactly as in the plugin. Where the plugin uses uint32
sample-rate truncation / integer division, we replicate it.

Run:   python3 sims/sim_audio_path.py
"""

import math
import random
import statistics
from dataclasses import dataclass, field

# ── Constants ported verbatim from the plugin ───────────────────────────────
CLOCK_HISTORY_SIZE = 64
DEFAULT_WINDOW_NS = 5_000_000_000          # clock-tracker.c
DEFAULT_EMA_ALPHA = 0.02                   # clock-tracker.c

AUDIO_BUF_MAX_FRAMES = 128                 # audio-buffer.h
DEFAULT_MIN_BUFFER_NS = 80_000_000         # 80ms   (JITTER_BUFFER_MS)
DEFAULT_MAX_BUFFER_NS = 500_000_000        # 500ms  (MAX_BUFFER_MS)

SR_WARMUP_NS = 5_000_000_000               # 5s
SR_HOLD_TIME_NS = 3_000_000_000            # 3s
SR_DEADZONE = 0.04                         # |rate-1| > 0.04
DEFAULT_FRAME_DUR_NS = 21_333_333          # 1024 / 48000 fallback in tick

SAMPLE_RATE = 48000
SAMPLES_PER_FRAME = 1024
# nominal frame duration in seconds (audio: 1024 samples @ 48kHz ~ 21.333ms)
NOMINAL_FRAME_DUR_S = SAMPLES_PER_FRAME / SAMPLE_RATE


def pts_of_frame(i):
    """Stream PTS (ns) of audio frame index i — monotonic content timeline."""
    return int(round(i * SAMPLES_PER_FRAME * 1_000_000_000 / SAMPLE_RATE))


# ─────────────────────────────────────────────────────────────────────────────
#  clock_tracker  (port of clock-tracker.c)
# ─────────────────────────────────────────────────────────────────────────────
class ClockTracker:
    def __init__(self):
        self.history = [None] * CLOCK_HISTORY_SIZE  # (stream_pts_ns, wall_ns)
        self.history_count = 0
        self.history_head = 0
        self.stream_rate = 1.0
        self.smoothed_rate = 1.0
        self.drift_ns = 0
        self.anchor_stream_ns = 0
        self.anchor_wall_ns = 0
        self.anchor_set = False
        self.window_ns = DEFAULT_WINDOW_NS
        self.ema_alpha = DEFAULT_EMA_ALPHA

    def reset(self):
        self.history = [None] * CLOCK_HISTORY_SIZE
        self.history_count = 0
        self.history_head = 0
        self.stream_rate = 1.0
        self.smoothed_rate = 1.0
        self.drift_ns = 0
        self.anchor_stream_ns = 0
        self.anchor_wall_ns = 0
        self.anchor_set = False

    def record(self, stream_pts_ns, wall_time_ns):
        if not self.anchor_set:
            self.anchor_stream_ns = stream_pts_ns
            self.anchor_wall_ns = wall_time_ns
            self.anchor_set = True

        idx = self.history_head % CLOCK_HISTORY_SIZE
        self.history[idx] = (stream_pts_ns, wall_time_ns)
        self.history_head += 1
        if self.history_count < CLOCK_HISTORY_SIZE:
            self.history_count += 1

        # Find oldest sample within window (scan newest -> back, break on window)
        oldest_idx = -1
        oldest_wall = wall_time_ns
        for i in range(self.history_count):
            si = (self.history_head - 1 - i + CLOCK_HISTORY_SIZE * 2) % CLOCK_HISTORY_SIZE
            sample = self.history[si]
            sample_wall = sample[1]
            if wall_time_ns - sample_wall > self.window_ns:
                break
            if sample_wall < oldest_wall:
                oldest_wall = sample_wall
                oldest_idx = si

        if oldest_idx >= 0:
            o_pts, o_wall = self.history[oldest_idx]
            wall_elapsed = wall_time_ns - o_wall
            stream_elapsed = stream_pts_ns - o_pts
            if wall_elapsed > 100_000_000:  # need at least 100ms
                self.stream_rate = stream_elapsed / wall_elapsed
                if self.stream_rate < 0.90:
                    self.stream_rate = 0.90
                if self.stream_rate > 1.10:
                    self.stream_rate = 1.10
                self.smoothed_rate = (
                    self.ema_alpha * self.stream_rate
                    + (1.0 - self.ema_alpha) * self.smoothed_rate
                )

        stream_elapsed = stream_pts_ns - self.anchor_stream_ns
        wall_elapsed = wall_time_ns - self.anchor_wall_ns
        self.drift_ns = wall_elapsed - stream_elapsed

    def get_smoothed_rate(self):
        return self.smoothed_rate


# ─────────────────────────────────────────────────────────────────────────────
#  audio_buffer  (port of audio-buffer.c)
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class Frame:
    frames: int
    sample_rate: int
    pts_ns: int
    valid: bool = True


class AudioBuffer:
    def __init__(self):
        self.frames = [None] * AUDIO_BUF_MAX_FRAMES
        self.read_pos = 0
        self.write_pos = 0
        self.count = 0
        self.min_buffer_ns = DEFAULT_MIN_BUFFER_NS
        self.max_buffer_ns = DEFAULT_MAX_BUFFER_NS
        self.primed = False
        self.total_buffered_ns = 0
        self.last_output_pts = -1
        self.frames_in = 0
        self.frames_out = 0
        self.frames_dropped = 0

    def reset(self):
        min_ns, max_ns = self.min_buffer_ns, self.max_buffer_ns
        self.__init__()
        self.min_buffer_ns = min_ns
        self.max_buffer_ns = max_ns

    @staticmethod
    def _frame_dur(frames, sr):
        if sr == 0:
            return 0
        return int(frames) * 1_000_000_000 // int(sr)

    def _recalc(self):
        total = 0
        for i in range(self.count):
            idx = (self.read_pos + i) % AUDIO_BUF_MAX_FRAMES
            f = self.frames[idx]
            if f and f.valid:
                total += self._frame_dur(f.frames, f.sample_rate)
        self.total_buffered_ns = total

    def push(self, frames, sample_rate, pts_ns):
        if self.count >= AUDIO_BUF_MAX_FRAMES:
            self.read_pos = (self.read_pos + 1) % AUDIO_BUF_MAX_FRAMES
            self.count -= 1
            self.frames_dropped += 1

        while self.count > 0 and self.total_buffered_ns > self.max_buffer_ns:
            self.read_pos = (self.read_pos + 1) % AUDIO_BUF_MAX_FRAMES
            self.count -= 1
            self.frames_dropped += 1
            self._recalc()

        self.frames[self.write_pos] = Frame(frames, sample_rate, pts_ns, True)
        self.write_pos = (self.write_pos + 1) % AUDIO_BUF_MAX_FRAMES
        self.count += 1
        self.frames_in += 1
        self.total_buffered_ns += self._frame_dur(frames, sample_rate)

        if not self.primed and self.total_buffered_ns >= self.min_buffer_ns:
            self.primed = True
        return True

    def pop(self):
        """Returns popped Frame or None (matches bool return + out ptr)."""
        if self.count == 0:
            return None
        if not self.primed:
            return None
        f = self.frames[self.read_pos]
        if not f or not f.valid:
            return None
        self.last_output_pts = f.pts_ns
        self.total_buffered_ns -= self._frame_dur(f.frames, f.sample_rate)
        if self.total_buffered_ns < 0:
            self.total_buffered_ns = 0
        self.read_pos = (self.read_pos + 1) % AUDIO_BUF_MAX_FRAMES
        self.count -= 1
        self.frames_out += 1
        return f

    def is_ready(self):
        return self.primed

    def level_ns(self):
        return self.total_buffered_ns


# ─────────────────────────────────────────────────────────────────────────────
#  Tunable knobs for prototype fixes (defaults = faithful plugin behavior)
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class Tuning:
    # SR correction
    sr_round_to: int = 100        # plugin rounds adjusted SR to nearest 100 Hz
    sr_deadzone: float = SR_DEADZONE
    sr_hold_ns: int = SR_HOLD_TIME_NS
    sr_continuous: bool = False   # FIX: continuous proportional correction
    sr_clamp: float = 0.05        # ±5%
    # drain
    max_pops_per_tick: int = 3    # plugin caps at 3
    # behind clamp
    behind_clamp_ns: int = 200_000_000


# ─────────────────────────────────────────────────────────────────────────────
#  The source: on_audio_frame (push path) + smooth_media_tick (drain)
# ─────────────────────────────────────────────────────────────────────────────
class SmoothMediaSource:
    def __init__(self, tuning=None, sync_pts=False):
        self.tuning = tuning or Tuning()
        self.sync_pts = sync_pts
        self.clock = ClockTracker()
        self.audio_buf = AudioBuffer()
        self.audio_buf.min_buffer_ns = DEFAULT_MIN_BUFFER_NS
        self.audio_buf.max_buffer_ns = DEFAULT_MAX_BUFFER_NS

        self.active = True
        self.first_audio = False
        self.stream_start_time = 0
        self.video_frames_out = 0  # we simulate audio-only -> stays 0

        self.audio_frames_out = 0
        self.audio_out_ts = 0
        self.audio_next_ts = 0
        self.prev_audio_pts = 0
        self.last_audio_pop_time = 0
        self.audio_frame_dur_ns = 0
        self.sr_hold_start = 0

        # ── instrumentation ──
        self.outputs = []          # list of (wall_now, out_ts, frames, sr)
        self.snap_events = []      # ('big_delta'|'behind', wall_now)
        self.underrun_ticks = 0    # ticks where we wanted to pop but buffer empty
        self.level_samples = []    # (wall_now, level_ns)

    # — push path —
    def on_audio_frame(self, pts_ns, wall_now, frames=SAMPLES_PER_FRAME,
                       sample_rate=SAMPLE_RATE):
        self.clock.record(pts_ns, wall_now)
        if not self.first_audio:
            self.first_audio = True
        self.audio_buf.push(frames, sample_rate, pts_ns)

    # — drain path (smooth_media_tick) —
    def tick(self, wall_now):
        if not (self.active and self.first_audio):
            return
        frame_dur = self.audio_frame_dur_ns
        if frame_dur <= 0:
            frame_dur = DEFAULT_FRAME_DUR_NS

        pops = 0
        wanted_to_pop_but_empty = False
        while pops < self.tuning.max_pops_per_tick:
            if self.last_audio_pop_time == 0:
                should_pop = self.audio_buf.is_ready()
            else:
                elapsed = wall_now - self.last_audio_pop_time
                should_pop = (elapsed >= frame_dur)
            if not should_pop:
                break

            buf_frame = self.audio_buf.pop()
            if buf_frame is None:
                # primed & demand existed but nothing to give -> underrun
                if self.audio_buf.is_ready():
                    wanted_to_pop_but_empty = True
                break

            if buf_frame.sample_rate > 0 and buf_frame.frames > 0:
                self.audio_frame_dur_ns = (
                    buf_frame.frames * 1_000_000_000 // buf_frame.sample_rate
                )

            if self.last_audio_pop_time == 0:
                self.last_audio_pop_time = wall_now
            else:
                self.last_audio_pop_time += frame_dur
                if wall_now - self.last_audio_pop_time > frame_dur * 2:
                    self.last_audio_pop_time = wall_now - frame_dur

            # ── Sample-rate adjustment ──
            rate = self.clock.get_smoothed_rate()
            adjusted_sample_rate = buf_frame.sample_rate
            in_warmup = (wall_now - self.stream_start_time) < SR_WARMUP_NS
            outside_deadzone = abs(rate - 1.0) > self.tuning.sr_deadzone

            if self.tuning.sr_continuous:
                # FIX: continuous proportional correction (no deadzone cliff,
                # no round-to-100 pitch step, no 3s hold latency).
                if not in_warmup:
                    raw = buf_frame.sample_rate * rate
                    adjusted_sample_rate = int(round(raw))
                    sr_min = int(buf_frame.sample_rate * (1 - self.tuning.sr_clamp))
                    sr_max = int(buf_frame.sample_rate * (1 + self.tuning.sr_clamp))
                    adjusted_sample_rate = max(sr_min, min(sr_max, adjusted_sample_rate))
            else:
                # faithful plugin behavior
                if outside_deadzone:
                    if self.sr_hold_start == 0:
                        self.sr_hold_start = wall_now
                else:
                    self.sr_hold_start = 0
                held_long_enough = (
                    self.sr_hold_start != 0
                    and (wall_now - self.sr_hold_start) >= self.tuning.sr_hold_ns
                )
                if (not in_warmup) and held_long_enough:
                    raw = int(buf_frame.sample_rate * rate)
                    r = self.tuning.sr_round_to
                    adjusted_sample_rate = ((raw + r // 2) // r) * r
                    sr_min = int(buf_frame.sample_rate * 0.95)
                    sr_max = int(buf_frame.sample_rate * 1.05)
                    if adjusted_sample_rate < sr_min:
                        adjusted_sample_rate = sr_min
                    if adjusted_sample_rate > sr_max:
                        adjusted_sample_rate = sr_max

            # ── Timestamp computation (sync_pts OFF path; video_frames_out==0) ──
            out_ts = self._compute_ts(buf_frame, wall_now)
            self.audio_out_ts = out_ts

            self.outputs.append((wall_now, out_ts, buf_frame.frames,
                                 adjusted_sample_rate))
            self.audio_frames_out += 1
            pops += 1

        if wanted_to_pop_but_empty:
            self.underrun_ticks += 1
        self.level_samples.append((wall_now, self.audio_buf.level_ns()))

    def _compute_ts(self, buf_frame, wall_now):
        t = self.tuning
        # sync_pts && video_frames_out>0 branch never taken (audio-only sim)
        if self.audio_frames_out == 0:
            out_ts = wall_now
            self.audio_next_ts = wall_now
            self.prev_audio_pts = buf_frame.pts_ns
            return out_ts

        pts_delta = buf_frame.pts_ns - self.prev_audio_pts
        self.prev_audio_pts = buf_frame.pts_ns

        if 0 < pts_delta < 500_000_000:
            self.audio_next_ts += pts_delta
        else:
            self.audio_next_ts = wall_now
            self.snap_events.append(('big_delta', wall_now))

        drift_error = wall_now - self.audio_next_ts
        if drift_error < 0:
            self.audio_next_ts += drift_error // 10
        elif drift_error > 100_000_000:
            self.audio_next_ts += drift_error // 100
        else:
            self.audio_next_ts += drift_error // 1000

        max_ahead = wall_now + 20_000_000
        if self.audio_next_ts > max_ahead:
            self.audio_next_ts = max_ahead

        if self.audio_next_ts < wall_now - t.behind_clamp_ns:
            self.audio_next_ts = wall_now
            self.snap_events.append(('behind', wall_now))

        return self.audio_next_ts


# ─────────────────────────────────────────────────────────────────────────────
#  Scenario generators -> list of (arrival_wall_ns, pts_ns) per audio frame
# ─────────────────────────────────────────────────────────────────────────────
NS = 1_000_000_000


def gen_constant_rate(duration_s, rate, jitter_sigma_ns=0, rng=None,
                      loss_prob=0.0):
    """Frame i has pts = i*frame_dur; arrival = pts/rate (+jitter).
    rate<1 => slow stream (arrivals spread out > realtime)."""
    rng = rng or random.Random(1234)
    n = int(duration_s / NOMINAL_FRAME_DUR_S)
    out = []
    for i in range(n):
        if loss_prob and rng.random() < loss_prob:
            continue  # dropped frame -> PTS hole
        pts = pts_of_frame(i)
        arrival = pts / rate
        if jitter_sigma_ns:
            arrival += rng.gauss(0, jitter_sigma_ns)
        out.append((int(arrival), pts))
    out.sort(key=lambda x: x[0])
    return out


def gen_bursty(duration_s, rate, burst_period_min_ms, burst_period_max_ms,
               rng=None):
    """Frames generated at `rate` but delivered in bursts: accumulate frames
    and release them all at burst boundaries (bonded-cellular buffering)."""
    rng = rng or random.Random(99)
    n = int(duration_s / NOMINAL_FRAME_DUR_S)
    out = []
    next_burst = 0.0
    pending = []
    for i in range(n):
        pts = pts_of_frame(i)
        ready_wall = pts / rate  # when it would arrive at steady rate (s)
        pending.append(pts)
        if ready_wall >= next_burst:
            for p in pending:
                out.append((int(next_burst * NS), p))
            pending = []
            gap = rng.uniform(burst_period_min_ms, burst_period_max_ms) / 1000.0
            next_burst = ready_wall + gap
    for p in pending:
        out.append((int(next_burst * NS), p))
    out.sort(key=lambda x: x[0])
    return out


def gen_stalls_v2(duration_s, rate, freezes_s, rng=None):
    """Stall model with catch-up (no permanent added latency).
    freezes_s: list of (start_s, dur_s). A frame whose natural arrival time t
    lands inside a freeze window [start, start+dur) is held and delivered as a
    catch-up burst at start+dur. Frames after the window resume at realtime."""
    n = int(duration_s / NOMINAL_FRAME_DUR_S)
    freezes = sorted(freezes_s)
    out = []
    for i in range(n):
        pts = pts_of_frame(i)
        arrival = pts / rate  # natural arrival (s)
        for (start, dur) in freezes:
            if start <= arrival < start + dur:
                arrival = start + dur
                break
        out.append((int(arrival * NS), pts))
    out.sort(key=lambda x: x[0])
    return out


# ─────────────────────────────────────────────────────────────────────────────
#  Metrics
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class Metrics:
    name: str
    n_out: int = 0
    disc_5ms: int = 0
    disc_20ms: int = 0
    max_disc_ms: float = 0.0
    snap_big_delta: int = 0
    snap_behind: int = 0
    sr_changes: int = 0
    sr_max_step: int = 0
    sr_min: int = 0
    sr_max: int = 0
    sr_oscillations: int = 0
    underrun_ticks: int = 0
    frames_dropped: int = 0
    mean_level_ms: float = 0.0
    max_level_ms: float = 0.0
    final_drift_ms: float = 0.0
    max_drift_ms: float = 0.0
    recovery_ms: list = field(default_factory=list)


def analyze(name, src, recovery_probe_walls=None):
    m = Metrics(name=name)
    outs = src.outputs
    m.n_out = len(outs)
    m.frames_dropped = src.audio_buf.frames_dropped
    m.underrun_ticks = src.underrun_ticks

    # output timestamp regularity
    expected = NOMINAL_FRAME_DUR_S * 1000.0  # ms per frame (nominal)
    for n in range(len(outs) - 1):
        spacing_ms = (outs[n + 1][1] - outs[n][1]) / 1e6
        # expected from frames[n] at nominal 48k
        exp_ms = outs[n][2] / SAMPLE_RATE * 1000.0
        d = abs(spacing_ms - exp_ms)
        if d > m.max_disc_ms:
            m.max_disc_ms = d
        if d > 5.0:
            m.disc_5ms += 1
        if d > 20.0:
            m.disc_20ms += 1

    for kind, _ in src.snap_events:
        if kind == 'big_delta':
            m.snap_big_delta += 1
        else:
            m.snap_behind += 1

    # SR behavior
    srs = [o[3] for o in outs]
    if srs:
        m.sr_min = min(srs)
        m.sr_max = max(srs)
        prev = srs[0]
        last_dir = 0
        for v in srs[1:]:
            if v != prev:
                m.sr_changes += 1
                step = abs(v - prev)
                if step > m.sr_max_step:
                    m.sr_max_step = step
                d = 1 if v > prev else -1
                if last_dir != 0 and d != last_dir:
                    m.sr_oscillations += 1
                last_dir = d
                prev = v

    # latency (buffer level)
    lvls = [l for (_, l) in src.level_samples]
    if lvls:
        m.mean_level_ms = statistics.mean(lvls) / 1e6
        m.max_level_ms = max(lvls) / 1e6

    # drift of out_ts vs wall_now
    drifts = [(o[1] - o[0]) / 1e6 for o in outs]  # out_ts - wall_now (ms)
    if drifts:
        m.final_drift_ms = drifts[-1]
        m.max_drift_ms = max(abs(d) for d in drifts)

    return m


def print_metrics(m):
    print(f"\n=== Scenario {m.name} ===")
    print(f"  outputs emitted ............. {m.n_out}")
    print(f"  ts discontinuities >5ms ..... {m.disc_5ms}")
    print(f"  ts discontinuities >20ms .... {m.disc_20ms}")
    print(f"  max ts discontinuity ........ {m.max_disc_ms:.2f} ms")
    print(f"  hard snap (big pts_delta) ... {m.snap_big_delta}")
    print(f"  hard snap (behind-clamp) .... {m.snap_behind}")
    print(f"  adjusted_sr changes ......... {m.sr_changes}")
    print(f"  adjusted_sr max step ........ {m.sr_max_step} Hz")
    print(f"  adjusted_sr range ........... [{m.sr_min}, {m.sr_max}] Hz")
    print(f"  adjusted_sr oscillations .... {m.sr_oscillations}")
    print(f"  underrun ticks .............. {m.underrun_ticks}")
    print(f"  frames dropped (overflow) ... {m.frames_dropped}")
    print(f"  mean buffer level ........... {m.mean_level_ms:.1f} ms")
    print(f"  max buffer level ............ {m.max_level_ms:.1f} ms")
    print(f"  out_ts vs wall drift (final). {m.final_drift_ms:.1f} ms")
    print(f"  out_ts vs wall drift (max) .. {m.max_drift_ms:.1f} ms")
    if m.recovery_ms:
        print(f"  stall recovery times ........ "
              f"{', '.join(f'{r:.0f}ms' for r in m.recovery_ms)}")


# ─────────────────────────────────────────────────────────────────────────────
#  Driver: merge arrival + tick events and run
# ─────────────────────────────────────────────────────────────────────────────
def run_scenario(name, frames, fps=60.0, tuning=None, sync_pts=False,
                 reset_at_s=None):
    """frames: list of (arrival_wall_ns, pts_ns). Runs the event loop."""
    src = SmoothMediaSource(tuning=tuning, sync_pts=sync_pts)
    tick_int = int(round(NS / fps))

    if not frames:
        return src, analyze(name, src)

    end_wall = max(f[0] for f in frames) + 2 * NS
    # build merged event timeline
    events = []
    for (aw, pts) in frames:
        events.append((aw, 0, pts))  # type 0 = arrival
    t = 0
    while t <= end_wall:
        events.append((t, 1, None))  # type 1 = tick
        t += tick_int
    events.sort(key=lambda e: (e[0], e[1]))  # arrivals (0) before ticks (1)

    did_reset = False
    reset_wall = int(reset_at_s * NS) if reset_at_s else None

    for (wall, typ, pts) in events:
        if reset_wall is not None and not did_reset and wall >= reset_wall:
            # simulate reconnect: clock_tracker_reset + audio_buffer_reset
            src.clock.reset()
            src.audio_buf.reset()
            src.first_audio = False
            src.audio_frames_out = 0
            src.audio_out_ts = 0
            src.audio_next_ts = 0
            src.prev_audio_pts = 0
            src.last_audio_pop_time = 0
            src.audio_frame_dur_ns = 0
            src.sr_hold_start = 0
            src.stream_start_time = wall
            did_reset = True
        if typ == 0:
            src.on_audio_frame(pts, wall)
        else:
            src.tick(wall)

    return src, analyze(name, src)


def compute_recovery(src, freeze_ends_s):
    """For each freeze end, time until output spacing returns to ~frame_dur."""
    outs = src.outputs
    exp = NOMINAL_FRAME_DUR_S
    recs = []
    for fe in freeze_ends_s:
        fe_ns = int(fe * NS)
        # find first output at/after freeze end, then time until 10 consecutive
        # spacings within 3ms of nominal
        start_idx = None
        for i, o in enumerate(outs):
            if o[0] >= fe_ns:
                start_idx = i
                break
        if start_idx is None:
            recs.append(0.0)
            continue
        good_run = 0
        rec_ns = 0
        for n in range(start_idx, len(outs) - 1):
            spacing = (outs[n + 1][1] - outs[n][1]) / NS
            if abs(spacing - exp) < 0.003:
                good_run += 1
                if good_run >= 10:
                    rec_ns = outs[n + 1][0] - fe_ns
                    break
            else:
                good_run = 0
        recs.append(rec_ns / 1e6)
    return recs


# ─────────────────────────────────────────────────────────────────────────────
#  Main
# ─────────────────────────────────────────────────────────────────────────────
def main():
    DUR = 120.0
    all_metrics = []

    def run_and_report(name, frames, **kw):
        for fps in (60.0, 30.0):
            tag = f"{name} @{int(fps)}fps"
            src, m = run_scenario(tag, frames, fps=fps, **kw)
            print_metrics(m)
            all_metrics.append((tag, m, src))

    print("#" * 70)
    print("# obs-smooth-media AUDIO PATH SIMULATION")
    print(f"# stream duration target: {DUR}s, 48kHz, 1024 samp/frame "
          f"({NOMINAL_FRAME_DUR_S*1000:.3f}ms)")
    print("#" * 70)

    rng = random.Random(2026)

    # A. Clean 1.0x
    run_and_report("A clean 1.000x", gen_constant_rate(DUR, 1.000))

    # B. Slow streams (classic stutter trigger)
    run_and_report("B1 slow 0.980x", gen_constant_rate(DUR, 0.980))
    run_and_report("B2 slow 0.970x", gen_constant_rate(DUR, 0.970))

    # C. Fast
    run_and_report("C fast 1.020x", gen_constant_rate(DUR, 1.020))

    # D. Bursty delivery
    run_and_report("D1 bursty 200-600ms", gen_bursty(DUR, 1.0, 200, 600,
                                                      rng=random.Random(11)))
    run_and_report("D2 bursty 400-600ms big", gen_bursty(DUR, 0.99, 400, 600,
                                                          rng=random.Random(12)))

    # E. Network stalls + catch-up
    freezes = [(20.0, 0.5), (40.0, 1.0), (60.0, 2.0), (85.0, 4.0)]
    fr_E = gen_stalls_v2(DUR, 1.0, freezes)
    for fps in (60.0, 30.0):
        tag = f"E stalls .5/1/2/4s @{int(fps)}fps"
        src, m = run_scenario(tag, fr_E, fps=fps)
        # catch-up model: freeze end (when burst arrives) is simply start+dur
        ends = [st + du for (st, du) in freezes]
        m.recovery_ms = compute_recovery(src, ends)
        print_metrics(m)
        all_metrics.append((tag, m, src))

    # F. Random jitter
    for sigma in (20, 50, 120):
        run_and_report(f"F jitter sigma={sigma}ms",
                       gen_constant_rate(DUR, 1.0,
                                         jitter_sigma_ns=sigma * 1_000_000,
                                         rng=random.Random(sigma)))

    # G. Packet loss / PTS gaps
    run_and_report("G loss 2%",
                   gen_constant_rate(DUR, 1.0, loss_prob=0.02,
                                     rng=random.Random(55)))
    run_and_report("G2 loss 5%",
                   gen_constant_rate(DUR, 1.0, loss_prob=0.05,
                                     rng=random.Random(56)))

    # H. Reconnect mid-stream
    run_and_report("H reconnect @60s", gen_constant_rate(DUR, 0.99),
                   reset_at_s=60.0)

    # I. Combined worst case: slow 0.985 + 50ms jitter + stalls + loss
    freezes_I = [(15.0, 1.0), (35.0, 2.0), (55.0, 1.5), (80.0, 2.0),
                 (100.0, 1.0)]
    base_I = gen_stalls_v2(DUR, 0.985, freezes_I)
    # add jitter + loss on top
    rngI = random.Random(2024)
    fr_I = []
    for (aw, pts) in base_I:
        if rngI.random() < 0.02:
            continue
        fr_I.append((aw + int(rngI.gauss(0, 50_000_000)), pts))
    fr_I.sort(key=lambda x: x[0])
    run_and_report("I worst-case combined", fr_I)

    # ── Prototype FIX comparison on worst-case (scenario I) ──
    print("\n" + "#" * 70)
    print("# PROTOTYPE FIX COMPARISON — scenario I (worst case), 60fps")
    print("#" * 70)
    fix = Tuning(sr_continuous=True, sr_round_to=1, sr_deadzone=0.0,
                 sr_hold_ns=0, max_pops_per_tick=8)
    src_base, m_base = run_scenario("I BASELINE", fr_I, fps=60.0)
    src_fix, m_fix = run_scenario("I FIXED", fr_I, fps=60.0, tuning=fix)
    print_metrics(m_base)
    print_metrics(m_fix)

    print("\n" + "=" * 70)
    print("BEFORE/AFTER (scenario I @60fps):")
    print(f"  {'metric':<28}{'baseline':>14}{'fixed':>14}")
    rows = [
        ("ts disc >5ms", m_base.disc_5ms, m_fix.disc_5ms),
        ("ts disc >20ms", m_base.disc_20ms, m_fix.disc_20ms),
        ("max ts disc (ms)", round(m_base.max_disc_ms, 1), round(m_fix.max_disc_ms, 1)),
        ("snap big_delta", m_base.snap_big_delta, m_fix.snap_big_delta),
        ("snap behind", m_base.snap_behind, m_fix.snap_behind),
        ("sr changes", m_base.sr_changes, m_fix.sr_changes),
        ("sr max step (Hz)", m_base.sr_max_step, m_fix.sr_max_step),
        ("sr oscillations", m_base.sr_oscillations, m_fix.sr_oscillations),
        ("underrun ticks", m_base.underrun_ticks, m_fix.underrun_ticks),
        ("frames dropped", m_base.frames_dropped, m_fix.frames_dropped),
        ("max buffer (ms)", round(m_base.max_level_ms, 1), round(m_fix.max_level_ms, 1)),
        ("max drift (ms)", round(m_base.max_drift_ms, 1), round(m_fix.max_drift_ms, 1)),
    ]
    for label, a, b in rows:
        print(f"  {label:<28}{str(a):>14}{str(b):>14}")
    print("=" * 70)


if __name__ == "__main__":
    main()
