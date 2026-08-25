#!/usr/bin/env python3
"""Deterministic executable specification for the live timing controller.

This intentionally mirrors the production equations at a higher level than
the native memory/thread tests. It fails when a stable slow/fast feed drains
the jitter buffer, produces non-monotonic timestamps, exceeds the latency
ceiling, or lets matched audio/video content drift out of lip sync.
"""

from __future__ import annotations

import bisect
import collections
import math
import random
import sys

NS = 1_000_000_000
MS = 1_000_000
AUDIO_FRAMES = 1024
AUDIO_SR = 48_000
AUDIO_DUR = AUDIO_FRAMES * NS // AUDIO_SR
TICK = NS // 60
MIN_TARGET = 80 * MS
TARGET_CAP = 750 * MS
HARD_CAP = 1_200 * MS
MARGIN = 250 * MS
SETTLE = 8 * NS


class Clock:
    def __init__(self) -> None:
        self.samples: collections.deque[tuple[int, int]] = collections.deque(
            maxlen=2048
        )
        self.rate = 1.0
        self.pending: tuple[int, int] | None = None
        self.pending_start = 0
        self.last_filter_wall = 0

    def commit(self, sample: tuple[int, int]) -> None:
        pts, wall = sample
        self.samples.append(sample)
        oldest = None
        for candidate in reversed(self.samples):
            if wall - candidate[1] > 5 * NS:
                break
            oldest = candidate
        if oldest and wall - oldest[1] > 100 * MS:
            measured = (pts - oldest[0]) / (wall - oldest[1])
            measured = max(0.90, min(1.10, measured))
            elapsed = (
                250 * MS
                if self.last_filter_wall == 0
                else wall - self.last_filter_wall
            )
            alpha = 1.0 - math.exp(-elapsed / (2.7 * NS))
            self.rate += alpha * (measured - self.rate)
            self.last_filter_wall = wall

    def record(self, pts: int, wall: int) -> None:
        if self.pending:
            last_pts, last_wall = self.pending
            if pts < last_pts or wall < last_wall:
                self.samples.clear()
                self.rate = 1.0
                self.pending = None
                self.pending_start = 0
                self.last_filter_wall = 0
            elif pts == last_pts or wall == last_wall:
                return
        current = (pts, wall)
        if self.pending is None:
            self.pending = current
            self.pending_start = wall
            return
        if wall - self.pending[1] >= 100 * MS or wall - self.pending_start >= 250 * MS:
            self.commit(self.pending)
            self.pending = current
            self.pending_start = wall
        else:
            self.pending = current


class Buffer:
    def __init__(self) -> None:
        self.frames: collections.deque[int] = collections.deque()
        self.level = 0
        self.target = MIN_TARGET
        self.maximum = MIN_TARGET + MARGIN
        self.jitter = 0
        self.delivery_gap = 0
        self.last_gap_decay = 0
        self.primed = False
        self.prev_arrival: int | None = None
        self.prev_pts: int | None = None
        self.dropped = 0
        self.max_seen = 0

    def push(self, pts: int, arrival: int) -> None:
        if self.prev_arrival is not None and self.prev_pts is not None:
            arrival_delta = arrival - self.prev_arrival
            delta = abs(
                arrival_delta - (pts - self.prev_pts)
            )
            self.jitter += (delta - self.jitter) // 16
            if self.last_gap_decay and arrival > self.last_gap_decay:
                self.delivery_gap = max(
                    0,
                    self.delivery_gap
                    - (arrival - self.last_gap_decay) // 20,
                )
            self.last_gap_decay = arrival
            self.delivery_gap = max(self.delivery_gap, arrival_delta)
        self.prev_arrival = arrival
        self.prev_pts = pts
        self.target = min(
            TARGET_CAP,
            max(
                MIN_TARGET,
                MIN_TARGET + 3 * self.jitter,
                self.delivery_gap + 40 * MS,
            ),
        )
        self.maximum = min(HARD_CAP, self.target + MARGIN)

        if len(self.frames) >= 512:
            self.frames.popleft()
            self.level -= AUDIO_DUR
            self.dropped += 1
        self.frames.append(pts)
        self.level += AUDIO_DUR
        while len(self.frames) > 1 and self.level > self.maximum:
            self.frames.popleft()
            self.level -= AUDIO_DUR
            self.dropped += 1
        self.primed = self.primed or self.level >= self.target
        self.max_seen = max(self.max_seen, self.level)

    def pop(self) -> int | None:
        if not self.primed or not self.frames:
            return None
        pts = self.frames.popleft()
        self.level -= AUDIO_DUR
        return pts

    def trim(self) -> None:
        while len(self.frames) > 1 and self.level - AUDIO_DUR >= self.target:
            self.frames.popleft()
            self.level -= AUDIO_DUR
            self.dropped += 1


def interpolate(samples: list[tuple[int, int]], pts: int) -> float | None:
    keys = [sample[0] for sample in samples]
    index = bisect.bisect_left(keys, pts)
    if index == 0:
        return float(samples[0][1]) if samples else None
    if index >= len(samples):
        return None
    p0, t0 = samples[index - 1]
    p1, t1 = samples[index]
    if p1 == p0:
        return float(t0)
    return t0 + (t1 - t0) * (pts - p0) / (p1 - p0)


def simulate(
    true_rate: float, fps: int, jitter_ms: float, duration_s: int = 90
) -> dict[str, float]:
    rng = random.Random(20260730 + fps + int(true_rate * 1000))
    events: list[tuple[int, int, int]] = []
    base_latency = 50 * MS

    last_arrival = 0
    audio_count = int(duration_s * NS / AUDIO_DUR)
    for index in range(audio_count):
        pts = index * AUDIO_DUR
        jitter = int(rng.gauss(0, jitter_ms * MS))
        arrival = max(last_arrival, int(pts / true_rate) + base_latency + jitter)
        last_arrival = arrival
        events.append((arrival, 0, pts))
    final_audio_arrival = last_arrival

    video_dur = NS // fps
    last_arrival = 0
    for index in range(duration_s * fps):
        pts = index * video_dur
        jitter = int(rng.gauss(0, jitter_ms * MS))
        arrival = max(last_arrival, int(pts / true_rate) + base_latency + jitter)
        last_arrival = arrival
        events.append((arrival, 1, pts))

    for wall in range(0, duration_s * NS + 2 * NS, TICK):
        events.append((wall, 2, 0))
    events.sort()

    clock = Clock()
    buffer = Buffer()
    audio_outputs: list[tuple[int, int]] = []
    video_outputs: list[tuple[int, int]] = []
    last_pop = 0
    audio_next = 0
    video_next = 0
    prev_video_pts = 0
    playback_ratio = 1.0
    sr_ratio = 1.0
    sr_slow = 1.0
    output_sample_rates: set[int] = set()
    initial_trimmed = False
    underruns = 0
    starved = False

    for wall, kind, pts in events:
        if kind == 0:
            buffer.push(pts, wall)
            if wall >= SETTLE:
                clock.record(pts, wall)
            continue

        if kind == 1:
            target = min(
                buffer.maximum, max(buffer.target, buffer.level)
            )
            if not video_outputs:
                video_next = wall + target
            else:
                delta = pts - prev_video_pts
                if 0 < delta < 500 * MS:
                    video_next += int(delta / playback_ratio)
                else:
                    video_next = wall + target
                drift_target = wall + target
                error = drift_target - video_next
                if error < 0:
                    video_next += int(error / 10)
                elif error > 100 * MS:
                    video_next += int(error / 100)
                else:
                    video_next += int(error / 1000)
                video_next = min(video_next, drift_target + 20 * MS)
                video_next = max(video_next, video_outputs[-1][1] + 1)
            prev_video_pts = pts
            video_outputs.append((pts, video_next))
            continue

        if not buffer.primed:
            continue
        if not initial_trimmed and wall >= SETTLE:
            buffer.trim()
            initial_trimmed = True

        measured = max(0.90, min(1.10, clock.rate))
        level_error = max(
            -0.5, min(0.5, (buffer.level - buffer.target) / buffer.target)
        )
        sr_slow += 0.0015 * (measured - sr_slow)
        base_ratio = 1.0 if wall < SETTLE else sr_slow
        occupancy_gain = (
            0.15 if level_error < 0 else (0.0 if wall < SETTLE else 0.08)
        )
        desired = max(
            0.90,
            min(1.10, base_ratio * (1.0 + occupancy_gain * level_error)),
        )
        pop_interval = int(AUDIO_DUR / sr_ratio)

        for _ in range(8):
            should_pop = (
                buffer.primed if last_pop == 0 else wall - last_pop >= pop_interval
            )
            if not should_pop:
                break
            audio_pts = buffer.pop()
            if audio_pts is None:
                if (
                    audio_outputs
                    and wall < final_audio_arrival
                    and not starved
                ):
                    underruns += 1
                    starved = True
                break
            starved = False
            if last_pop == 0:
                last_pop = wall
            else:
                last_pop += pop_interval
                if wall - last_pop > 2 * pop_interval:
                    last_pop = wall - pop_interval

            slew = (
                0.0015
                if wall < SETTLE
                and level_error < -0.25
                and desired < sr_ratio
                else 0.0005
            )
            sr_ratio += max(-slew, min(slew, desired - sr_ratio))
            output_frames = max(1, round(AUDIO_FRAMES / sr_ratio))
            output_sample_rates.add(AUDIO_SR)
            playback_ratio = sr_ratio

            if not audio_outputs:
                audio_next = wall
            else:
                audio_next += output_frames * NS // AUDIO_SR
                error = wall - audio_next
                if error < 0:
                    audio_next += int(error / 10)
                elif error > 100 * MS:
                    audio_next += int(error / 100)
                else:
                    audio_next += int(error / 1000)
                audio_next = min(audio_next, wall + 20 * MS)
                if audio_next < wall - 200 * MS:
                    audio_next = wall
                audio_next = max(audio_next, audio_outputs[-1][1] + 1)
            audio_outputs.append((audio_pts, audio_next))

    audio_ts = [ts for _, ts in audio_outputs]
    video_ts = [ts for _, ts in video_outputs]
    assert all(a < b for a, b in zip(audio_ts, audio_ts[1:])), "audio timestamp reversal"
    assert output_sample_rates == {AUDIO_SR}, "output sample rate changed"
    assert all(a <= b for a, b in zip(video_ts, video_ts[1:])), "video timestamp reversal"
    stable_video_gaps = [
        current[1] - previous[1]
        for previous, current in zip(video_outputs, video_outputs[1:])
        if current[0] >= 20 * NS
    ]
    assert min(stable_video_gaps) > video_dur // 5, "collapsed stable video timestamps"
    assert max(stable_video_gaps) < video_dur * 4, "unstable video timestamp gap"
    assert buffer.max_seen <= HARD_CAP + AUDIO_DUR, "latency ceiling exceeded"
    assert underruns == 0, f"avoidable stable-feed underruns: {underruns}"

    sync_errors = []
    for pts, video_ts_value in video_outputs:
        if pts < 20 * NS or pts > (duration_s - 2) * NS:
            continue
        audio_ts_value = interpolate(audio_outputs, pts)
        if audio_ts_value is not None:
            sync_errors.append((video_ts_value - audio_ts_value) / MS)
    assert sync_errors, "no matched A/V samples"
    max_sync = max(abs(error) for error in sync_errors)
    assert max_sync < 40.0, f"A/V sync exceeded 40 ms: {max_sync:.2f} ms"

    return {
        "max_sync_ms": max_sync,
        "max_buffer_ms": buffer.max_seen / MS,
        "clock_rate": clock.rate,
        "drops": float(buffer.dropped),
    }


def main() -> int:
    scenarios = [
        (1.00, 30, 0.0),
        (1.00, 60, 5.0),
        (0.99, 60, 5.0),
        (0.98, 30, 10.0),
        (0.97, 60, 15.0),
        (1.03, 60, 10.0),
    ]
    for rate, fps, jitter in scenarios:
        metrics = simulate(rate, fps, jitter)
        print(
            f"rate={rate:.2f} fps={fps} jitter={jitter:.0f}ms "
            f"sync={metrics['max_sync_ms']:.1f}ms "
            f"buffer={metrics['max_buffer_ms']:.0f}ms "
            f"measured={metrics['clock_rate']:.4f}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"timing acceptance failed: {error}", file=sys.stderr)
        raise SystemExit(1)
