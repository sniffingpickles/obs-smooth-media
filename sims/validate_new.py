"""
Validation of the NEW closed-loop audio controller in obs-smooth-media.
Faithful pure-math port of the rewritten logic (clock tracker, adaptive
jitter buffer, rate-paced drain, continuous slew-limited sample-rate match).

Drives adversarial IRL scenarios (slow/fast drift, jitter, bursts, multi-second
stalls) and reports:
  - buffer health (avg/min level, % empty, underruns)  -> stutter proxy
  - output-timestamp regularity vs sample duration       -> audible-glitch proxy
  - sample-rate ratio trajectory (smoothness / steps)    -> pitch-step proxy
  - recovery time after stalls
"""
import random, statistics

NS = 1_000_000_000
FS = 1024
SR = 48000
FRAME_DUR = FS * NS // SR                      # 21_333_333 ns

# clock tracker
HIST = 320
WINDOW = 5 * NS
ALPHA = 0.008

# adaptive buffer
MIN_NS = 80 * NS // 1000
TARGET_CAP = 600 * NS // 1000
HARD_CAP = 1200 * NS // 1000
MARGIN = 250 * NS // 1000
CRED_STEP = 60 * NS // 1000
CRED_CAP = 400 * NS // 1000
CRED_DECAY = 500_000
JCOEF = 3

# tick / SR
WARMUP = 8 * NS
SLEW = 0.0005
POP_CAP = 8
SR_DEADBAND_HZ = 40.0
SR_SLOW_ALPHA = 0.0015
SR_MIN_HOLD = 2 * NS
CLOCK_SETTLE = 2 * NS
STALL_GAP = 500 * NS // 1000
SR_STABLE_JITTER = 50 * NS // 1000


class Clock:
    def __init__(self):
        self.h = []
        self.smoothed = 1.0

    def record(self, pts, wall):
        self.h.append((pts, wall))
        if len(self.h) > HIST:
            self.h.pop(0)
        oldest = None
        for (p, w) in reversed(self.h):
            if wall - w > WINDOW:
                break
            oldest = (p, w)
        if oldest:
            we = wall - oldest[1]
            se = pts - oldest[0]
            if we > 100_000_000:
                r = max(0.90, min(1.10, se / we))
                self.smoothed += ALPHA * (r - self.smoothed)


class Buf:
    def __init__(self):
        self.q = []           # list of pts
        self.level = 0
        self.primed = False
        self.target = MIN_NS
        self.jit = 0
        self.cred = 0
        self.pa = self.pp = 0
        self.have = False
        self.dropped = 0

    def recalc(self):
        t = MIN_NS + JCOEF * self.jit + self.cred
        t = max(MIN_NS, min(TARGET_CAP, t))
        self.target = t
        self.maxn = min(HARD_CAP, t + MARGIN)

    def push(self, pts, arrival):
        if self.have:
            d = abs((arrival - self.pa) - (pts - self.pp))
            self.jit += (d - self.jit) // 16
            if self.jit < 0:
                self.jit = 0
        self.pa, self.pp, self.have = arrival, pts, True
        if self.cred > 0:
            self.cred = max(0, self.cred - CRED_DECAY)
        self.recalc()
        if len(self.q) >= 128:
            self.q.pop(0); self.level -= FRAME_DUR; self.dropped += 1
        while self.q and self.level > self.maxn:
            self.q.pop(0); self.level -= FRAME_DUR; self.dropped += 1
        self.q.append(pts); self.level += FRAME_DUR
        if not self.primed and self.level >= self.target:
            self.primed = True

    def pop(self):
        if not self.primed or not self.q:
            return None
        pts = self.q.pop(0); self.level -= FRAME_DUR
        return pts

    def note_underrun(self):
        self.cred = min(CRED_CAP, self.cred + CRED_STEP); self.recalc()


def run(name, rate=1.0, jitter_ms=0.0, stalls=(), fps=60, dur=180, seed=1,
        src_sr=44100, connect_burst_s=0.0, settle=True):
    rng = random.Random(seed)
    clk, buf = Clock(), Buf()
    buf.recalc()
    global FRAME_DUR
    FRAME_DUR = FS * NS // src_sr

    # producer arrival schedule. PTS is monotonic (FFmpeg emits decoded audio
    # in order); jitter perturbs *arrival* time but in-order delivery means a
    # late frame holds back its successors and an early frame waits for its
    # predecessor (head-of-line) — so PTS order is preserved, jitter shows up
    # as bunching, which is exactly what the jitter buffer must absorb.
    n = int(dur * NS / FRAME_DUR)
    arr = []
    step = FRAME_DUR / rate
    prev_push = 0
    burst_frames = int(connect_burst_s * NS / FRAME_DUR)
    for k in range(n):
        pts = k * FRAME_DUR
        base = k * step
        # connect burst: the first connect_burst_s of media is dumped by the
        # server in ~0.3s wall (SRT buffer flush)
        if k < burst_frames:
            base = 0.3 * NS * (k / max(1, burst_frames))
        for (ss, sd) in stalls:
            if ss * NS <= base < (ss + sd) * NS:
                base = (ss + sd) * NS + (base - ss * NS) * 0.05  # catch-up burst
        jit = rng.gauss(0, jitter_ms * 1e6) if jitter_ms else 0
        avail = max(0, int(base + jit))
        avail = max(avail, prev_push)   # in-order: cannot precede predecessor
        prev_push = avail
        arr.append((avail, pts))
    # already in non-decreasing arrival order by construction

    ai = 0
    last_pop = 0
    sr_ratio = 1.0
    sr_slow = 1.0
    last_sr_change = 0
    clock_skip_until = CLOCK_SETTLE
    last_arrival = 0
    declared_sr = 0
    sr_changes = 0          # each = an OBS resampler reset = a click
    sr_changes_steady = 0   # changes after warmup+2s
    audio_next = 0
    prev_pts = 0
    frames_out = 0
    underruns = 0
    levels, out_ts_list, sr_list = [], [], []
    stream_start = 0
    frame_dur_est = FRAME_DUR

    tick = NS // fps
    end = dur * NS
    t = 0
    while t <= end:
        while ai < len(arr) and arr[ai][0] <= t:
            a, pts = arr[ai]; ai += 1
            if last_arrival != 0 and t - last_arrival > STALL_GAP:
                clock_skip_until = t + CLOCK_SETTLE   # re-settle after stall
            last_arrival = t
            if (not settle) or t >= clock_skip_until:
                clk.record(pts, t)
            buf.push(pts, t)

        if buf.primed or frames_out > 0:
            rate_c = max(0.90, min(1.10, clk.smoothed))
            lvl_err = 0.0
            if buf.target > 0:
                lvl_err = max(-0.5, min(0.0, (buf.level - buf.target) / buf.target))
            pop_interval = (frame_dur_est / rate_c) * (1 - 0.15 * lvl_err)
            sr_slow += SR_SLOW_ALPHA * (rate_c - sr_slow)
            in_warmup = (t - stream_start) < WARMUP
            desired = 1.0 if in_warmup else max(0.95, min(1.05, sr_slow))

            pops = 0
            while pops < POP_CAP:
                if last_pop == 0:
                    should = buf.primed
                else:
                    should = (t - last_pop) >= pop_interval
                if not should:
                    break
                pts = buf.pop()
                if pts is None:
                    if frames_out > 0:
                        underruns += 1; buf.note_underrun()
                    break
                if last_pop == 0:
                    last_pop = t
                else:
                    last_pop += pop_interval
                    if t - last_pop > 2 * pop_interval:
                        last_pop = t - pop_interval
                dr = max(-SLEW, min(SLEW, desired - sr_ratio))
                sr_ratio += dr

                # deadband + min-hold held declared sample rate (= resampler resets)
                exact = src_sr * sr_ratio
                change = False
                if declared_sr == 0:
                    change = True
                elif (abs(exact - declared_sr) >= SR_DEADBAND_HZ and
                      (t - last_sr_change) >= SR_MIN_HOLD and
                      buf.jit < SR_STABLE_JITTER):
                    change = True
                if change:
                    new_sr = int(exact + 0.5)
                    if new_sr != declared_sr:
                        sr_changes += 1
                        if t > WARMUP + 2 * NS:
                            sr_changes_steady += 1
                    declared_sr = new_sr
                    last_sr_change = t

                # non-sync timestamp path
                if frames_out == 0:
                    out_ts = t; audio_next = t; prev_pts = pts
                else:
                    pd = pts - prev_pts; prev_pts = pts
                    if 0 < pd < 500_000_000:
                        audio_next += pd
                    else:
                        audio_next = t
                    de = t - audio_next
                    if de < 0:
                        audio_next += de // 10
                    elif de > 100_000_000:
                        audio_next += de // 100
                    else:
                        audio_next += de // 1000
                    if audio_next > t + 20_000_000:
                        audio_next = t + 20_000_000
                    if audio_next < t - 200_000_000:
                        audio_next = t
                    out_ts = audio_next
                out_ts_list.append(out_ts)
                sr_list.append(declared_sr)
                frames_out += 1
                pops += 1
            levels.append(buf.level)
        t += tick

    # metrics
    avg = statistics.mean(levels) / 1e6 if levels else 0
    mn = min(levels) / 1e6 if levels else 0
    pe = 100 * sum(1 for l in levels if l == 0) / len(levels) if levels else 0
    # output timestamp regularity: spacing vs expected media duration
    exp = FRAME_DUR
    disc20 = disc5 = 0
    for i in range(1, len(out_ts_list)):
        gap = out_ts_list[i] - out_ts_list[i - 1]
        dev = abs(gap - exp)
        if dev > 20_000_000: disc20 += 1
        elif dev > 5_000_000: disc5 += 1
    sr_min = min(sr_list) if sr_list else 1
    sr_max = max(sr_list) if sr_list else 1
    print(f"{name:34s} avg_buf={avg:4.0f}ms empty={pe:4.1f}% "
          f"under={underruns:4d} drop={buf.dropped:4d} "
          f"disc>20ms={disc20:3d} CLICKS(sr_resets)={sr_changes:4d} steady={sr_changes_steady:3d} "
          f"sr=[{sr_min},{sr_max}] tgt={buf.target/1e6:.0f}ms")


if __name__ == "__main__":
    print("NEW controller — buffer health + output regularity (lower under/disc = better):\n")
    run("clean 1.00x", 1.00)
    run("slow 0.99x", 0.99)
    run("slow 0.98x", 0.98)
    run("slow 0.97x", 0.97)
    run("fast 1.02x", 1.02)
    run("slow 0.98x + 50ms jitter", 0.98, 50)
    run("slow 0.98x + 120ms jitter", 0.98, 120)
    run("0.98x + 1s stall @30,80,130", 0.98, 30, stalls=[(30,1),(80,1),(130,1)])
    run("0.98x + 2s stalls", 0.98, 30, stalls=[(40,2),(90,2),(140,2)])
    run("0.98x + 4s stall", 0.98, 50, stalls=[(60,4)])
    run("WORST: .985x+50ms+2s stalls", 0.985, 50, stalls=[(35,2),(85,2),(135,1.5)])
    run("clean 1.00x @30fps", 1.00, fps=30)
    run("slow 0.98x @30fps", 0.98, fps=30)

    print("\nPost-connect burst (SRT flush) — settle gate OFF vs ON:")
    run("connect-burst, settle OFF", 1.001, connect_burst_s=2.0, settle=False, dur=90)
    run("connect-burst, settle ON ", 1.001, connect_burst_s=2.0, settle=True, dur=90)
