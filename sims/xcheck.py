"""
Independent cross-check of three historical timing-controller failure modes.
This is a pure-math port of the relevant plugin logic.

HISTORICAL MODEL: this script intentionally preserves earlier constants and
logic. Use tests/test-timing-model.py for validation of the current design.

  (1) clock_tracker effective measurement window vs the intended 5s
  (2) jitter-buffer drain under a sustained ~0.98x "slightly slow" stream
      (the exact case the plugin exists to fix) with the real tick pop pacing
  (3) sample-rate correction step size at the 4% deadzone boundary
"""

NS = 1_000_000_000
FRAME_SAMPLES = 1024
SR = 48000
FRAME_DUR_NS = FRAME_SAMPLES * NS // SR          # 21_333_333 ns
HISTORY = 64
WINDOW_NS = 5 * NS
EMA_ALPHA = 0.02
JITTER_MIN_NS = 80 * NS // 1000                  # 80 ms prime
JITTER_MAX_NS = 500 * NS // 1000                 # 500 ms cap
DEADZONE = 0.04
SR_HOLD_NS = 3 * NS
SR_WARMUP_NS = 5 * NS


# ---------------- (1) effective clock window -------------------------------
def effective_window(rate):
    wall_step = FRAME_DUR_NS / rate              # arrival spacing (slower => bigger)
    span_ns = (HISTORY - 1) * wall_step
    print(f"  rate={rate}: arrival spacing={wall_step/1e6:.3f}ms, "
          f"64-sample history spans {span_ns/1e6:.0f}ms "
          f"(intended window=5000ms) -> "
          f"{'TRUNCATED to ' + str(round(span_ns/1e6)) + 'ms' if span_ns < WINDOW_NS else 'ok'}")


# ---------------- (2) jitter buffer drain under slow stream ----------------
class Clock:
    def __init__(self):
        self.hist = []        # (pts, wall)
        self.smoothed = 1.0
        self.rate = 1.0
        self.anchored = False

    def record(self, pts, wall):
        self.hist.append((pts, wall))
        if len(self.hist) > HISTORY:
            self.hist.pop(0)
        # oldest within window
        oldest = None
        for (p, w) in reversed(self.hist):
            if wall - w > WINDOW_NS:
                break
            oldest = (p, w)
        if oldest:
            we = wall - oldest[1]
            se = pts - oldest[0]
            if we > 100_000_000:
                r = se / we
                r = max(0.90, min(1.10, r))
                self.rate = r
                self.smoothed += EMA_ALPHA * (r - self.smoothed)


def simulate(rate, jitter_ms=0.0, duration_s=120, seed=1):
    import random
    rng = random.Random(seed)
    clock = Clock()
    # buffer holds frame PTS list; level tracked in ns
    buf = []
    level = 0
    primed = False

    # producer arrivals
    wall_step = FRAME_DUR_NS / rate
    n_frames = int(duration_s * NS / FRAME_DUR_NS)
    arrivals = []
    for k in range(n_frames):
        jit = rng.gauss(0, jitter_ms * 1e6) if jitter_ms else 0
        arrivals.append((max(0, int(k * wall_step + jit)), k * FRAME_DUR_NS))
    arrivals.sort()
    ai = 0

    last_pop = 0
    underruns = 0
    pops = 0
    levels = []
    sr_changes = 0
    last_adj_sr = SR
    sr_hold_start = 0
    stream_start = 0

    tick_dt = NS // 60
    wall = 0
    end = duration_s * NS
    while wall <= end:
        # push all arrivals due
        while ai < len(arrivals) and arrivals[ai][0] <= wall:
            _, pts = arrivals[ai]
            ai += 1
            clock.record(pts, wall)
            # overflow drop-oldest
            if len(buf) >= 128:
                buf.pop(0); level -= FRAME_DUR_NS
            while buf and level > JITTER_MAX_NS:
                buf.pop(0); level -= FRAME_DUR_NS
            buf.append(pts); level += FRAME_DUR_NS
            if not primed and level >= JITTER_MIN_NS:
                primed = True

        # tick drain (up to 3 pops)
        n = 0
        while n < 3:
            if last_pop == 0:
                should = primed and len(buf) > 0
            else:
                should = (wall - last_pop) >= FRAME_DUR_NS
            if not should:
                break
            if not (primed and buf):
                if primed:
                    underruns += 1
                break
            buf.pop(0); level -= FRAME_DUR_NS; pops += 1
            if last_pop == 0:
                last_pop = wall
            else:
                last_pop += FRAME_DUR_NS
                if wall - last_pop > 2 * FRAME_DUR_NS:
                    last_pop = wall - FRAME_DUR_NS

            # SR correction
            r = clock.smoothed
            in_warmup = (wall - stream_start) < SR_WARMUP_NS
            outside = abs(r - 1.0) > DEADZONE
            if outside:
                if sr_hold_start == 0:
                    sr_hold_start = wall
            else:
                sr_hold_start = 0
            held = sr_hold_start != 0 and (wall - sr_hold_start) >= SR_HOLD_NS
            adj = SR
            if not in_warmup and held:
                raw = int(SR * r)
                adj = ((raw + 50) // 100) * 100
                adj = max(int(SR * 0.95), min(int(SR * 1.05), adj))
            if adj != last_adj_sr:
                sr_changes += 1
                last_adj_sr = adj
            n += 1
        levels.append(level)

        wall += tick_dt

    import statistics
    avg_level = statistics.mean(levels) / 1e6
    min_level = min(levels) / 1e6
    pct_empty = 100.0 * sum(1 for l in levels if l == 0) / len(levels)
    print(f"  rate={rate} jitter={jitter_ms}ms: "
          f"smoothed_rate={clock.smoothed:.4f} "
          f"avg_buf={avg_level:.0f}ms min_buf={min_level:.0f}ms "
          f"empty={pct_empty:.1f}% underruns={underruns} sr_changes={sr_changes} "
          f"final_adj_sr={last_adj_sr}")


# ---------------- (3) SR step at deadzone boundary -------------------------
def sr_step():
    for r in [0.999, 0.97, 0.961, 0.959, 0.95, 0.94]:
        outside = abs(r - 1.0) > DEADZONE
        if outside:
            raw = int(SR * r)
            adj = ((raw + 50) // 100) * 100
            adj = max(int(SR * 0.95), min(int(SR * 1.05), adj))
        else:
            adj = SR  # no correction inside deadzone
        print(f"  rate={r}: corrected={outside} adj_sr={adj} "
              f"({'STEP ' + str(SR-adj) + 'Hz = ' + format(100*(SR-adj)/SR,'.2f') + '%% pitch jump' if outside else 'NO correction (relies on buffer)'})")


if __name__ == "__main__":
    print("\n(1) Clock tracker effective measurement window:")
    for r in (1.0, 0.98, 0.95):
        effective_window(r)

    print("\n(2) Jitter buffer under sustained / jittery streams:")
    simulate(1.00, 0)
    simulate(0.99, 0)
    simulate(0.98, 0)
    simulate(0.97, 0)
    simulate(0.98, 50)
    simulate(0.95, 0)

    print("\n(3) Sample-rate correction step at the 4% deadzone boundary:")
    sr_step()
