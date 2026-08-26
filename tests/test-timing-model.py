#!/usr/bin/env python3
"""Deterministic acceptance model for live pacing and stall recovery."""

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
OUTPUT_LEAD = 60 * MS
MIN_OUTPUT_LEAD = 5 * MS
MAX_OUTPUT_LEAD = 250 * MS


class Clock:
    def __init__(self) -> None:
        self.samples: collections.deque[tuple[int, int]] = collections.deque(
            maxlen=2048
        )
        self.rate = 1.0
        self.pending: tuple[int, int] | None = None
        self.pending_start = 0
        self.last_filter_wall = 0

    def reset(self) -> None:
        self.samples.clear()
        self.rate = 1.0
        self.pending = None
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
                self.reset()
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
        self.underrun_credit = 0
        self.last_gap_decay = 0
        self.primed = False
        self.prev_arrival: int | None = None
        self.prev_pts: int | None = None
        self.dropped = 0
        self.max_seen = 0

    def recalculate(self) -> None:
        target = max(MIN_TARGET, MIN_TARGET + 3 * self.jitter)
        target += self.underrun_credit
        target = max(target, self.delivery_gap + 40 * MS)
        self.target = min(TARGET_CAP, target)
        self.maximum = min(HARD_CAP, self.target + MARGIN)

    def push(self, pts: int, arrival: int) -> None:
        if self.prev_arrival is not None and self.prev_pts is not None:
            arrival_delta = arrival - self.prev_arrival
            delta = abs(arrival_delta - (pts - self.prev_pts))
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
        self.underrun_credit = max(0, self.underrun_credit - 500_000)
        self.recalculate()

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

    def note_underrun(self) -> None:
        self.primed = False
        self.underrun_credit = min(400 * MS, self.underrun_credit + 60 * MS)
        self.recalculate()

    def note_discontinuity(self) -> None:
        self.prev_arrival = None
        self.prev_pts = None
        self.delivery_gap = 0
        self.last_gap_decay = 0
        self.jitter = 0
        self.recalculate()

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
    true_rate: float,
    fps: int,
    jitter_ms: float,
    *,
    adaptive: bool,
    duration_s: int = 90,
    outage: tuple[int, int] | None = None,
) -> dict[str, float]:
    rng = random.Random(20260826 + fps + int(true_rate * 1000))
    events: list[tuple[int, int, int]] = []
    base_latency = 50 * MS

    def delayed(pts: int, arrival: int) -> int:
        if not outage:
            return arrival
        start, length = outage
        if start * NS <= pts < (start + length) * NS:
            return max(arrival, (start + length) * NS + base_latency)
        return arrival

    last_arrival = 0
    audio_count = int(duration_s * NS / AUDIO_DUR)
    for index in range(audio_count):
        pts = index * AUDIO_DUR
        jitter = int(rng.gauss(0, jitter_ms * MS))
        arrival = int(pts / true_rate) + base_latency + jitter
        arrival = delayed(pts, arrival)
        arrival = max(last_arrival, arrival)
        last_arrival = arrival
        events.append((arrival, 0, pts))
    final_audio_arrival = last_arrival

    video_dur = NS // fps
    last_arrival = 0
    for index in range(duration_s * fps):
        pts = index * video_dur
        jitter = int(rng.gauss(0, jitter_ms * MS))
        arrival = int(pts / true_rate) + base_latency + jitter
        arrival = delayed(pts, arrival)
        arrival = max(last_arrival, arrival)
        last_arrival = arrival
        events.append((arrival, 1, pts))

    for wall in range(0, duration_s * NS + 2 * NS, TICK):
        events.append((wall, 2, 0))
    events.sort()

    clock = Clock()
    buffer = Buffer()
    audio_outputs: list[tuple[int, int]] = []
    video_outputs: list[tuple[int, int]] = []
    output_leads: list[int] = []
    output_frame_counts: set[int] = set()
    ratios: list[float] = []
    last_pop = 0
    audio_next = 0
    video_next = 0
    prev_video_pts = 0
    prev_audio_arrival = 0
    playback_ratio = 1.0
    sr_ratio = 1.0
    sr_slow = 1.0
    skip_clock_until = SETTLE
    initial_trimmed = False
    rebuffering = False
    force_video_reanchor = False
    audio_anchor_pts: int | None = None
    audio_anchor_ts = 0
    underruns = 0
    recoveries = 0
    resyncs = 0

    for wall, kind, pts in events:
        if kind == 0:
            if prev_audio_arrival and wall - prev_audio_arrival > 2 * NS:
                clock.reset()
                buffer.note_discontinuity()
                skip_clock_until = wall + SETTLE
                initial_trimmed = False
                force_video_reanchor = True
            prev_audio_arrival = wall
            buffer.push(pts, wall)
            if wall >= skip_clock_until:
                clock.record(pts, wall)
            continue

        if kind == 1:
            if rebuffering:
                continue
            target = min(buffer.maximum, max(buffer.target, buffer.level))
            target += OUTPUT_LEAD
            pts_mapped = False
            if audio_anchor_pts is not None:
                mapped = audio_anchor_ts + int(
                    (pts - audio_anchor_pts) / playback_ratio
                )
                mapped = max(mapped, wall + MIN_OUTPUT_LEAD)
                if mapped <= wall + 1_500 * MS:
                    video_next = mapped
                    pts_mapped = True
                    force_video_reanchor = False
            if not pts_mapped and (not video_outputs or force_video_reanchor):
                video_next = wall + target
                force_video_reanchor = False
            elif not pts_mapped:
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
            if video_outputs and video_next <= video_outputs[-1][1]:
                continue
            prev_video_pts = pts
            video_outputs.append((pts, video_next))
            continue

        if not buffer.primed:
            continue
        if not initial_trimmed and wall >= skip_clock_until:
            buffer.trim()
            initial_trimmed = True

        measured = max(0.90, min(1.10, clock.rate))
        level_error = max(
            -0.5, min(0.5, (buffer.level - buffer.target) / buffer.target)
        )
        sr_slow += 0.0015 * (measured - sr_slow)
        desired = 1.0
        if adaptive:
            base = 1.0 if wall < skip_clock_until else sr_slow
            desired = max(0.98, min(1.02, base * (1.0 + 0.04 * level_error)))
        else:
            sr_ratio = 1.0
            playback_ratio = 1.0
        pop_interval = int(AUDIO_DUR / sr_ratio)

        for _ in range(8):
            should_pop = buffer.primed if last_pop == 0 else wall - last_pop >= pop_interval
            if not should_pop:
                break
            audio_pts = buffer.pop()
            if audio_pts is None:
                if audio_outputs and wall < final_audio_arrival and not rebuffering:
                    buffer.note_underrun()
                    underruns += 1
                    rebuffering = True
                    audio_anchor_pts = None
                    last_pop = 0
                    sr_ratio = 1.0
                    playback_ratio = 1.0
                    sr_slow = 1.0
                    clock.reset()
                    skip_clock_until = wall + SETTLE
                    initial_trimmed = True
                    force_video_reanchor = True
                break

            recovered = rebuffering
            if recovered:
                recoveries += 1
                last_pop = 0
                sr_ratio = 1.0
                playback_ratio = 1.0
                force_video_reanchor = True
                initial_trimmed = True

            if last_pop == 0:
                last_pop = wall
            else:
                last_pop += pop_interval
                if wall - last_pop > 2 * pop_interval:
                    last_pop = wall - pop_interval

            if adaptive and not recovered:
                sr_ratio += max(-0.0005, min(0.0005, desired - sr_ratio))
            else:
                sr_ratio = 1.0
            playback_ratio = sr_ratio
            output_frames = round(AUDIO_FRAMES / sr_ratio) if adaptive else AUDIO_FRAMES
            output_frame_counts.add(output_frames)
            ratios.append(sr_ratio)

            audio_step = output_frames * NS // AUDIO_SR
            timeline_resync = not audio_outputs or recovered
            if not timeline_resync:
                candidate = audio_next + audio_step
                timeline_resync = not (
                    wall + MIN_OUTPUT_LEAD <= candidate <= wall + MAX_OUTPUT_LEAD
                )
            if timeline_resync:
                audio_next = wall + OUTPUT_LEAD
                if audio_outputs:
                    resyncs += 1
                    force_video_reanchor = True
            else:
                audio_next += audio_step
            if audio_outputs:
                audio_next = max(audio_next, audio_outputs[-1][1] + 1)
            audio_outputs.append((audio_pts, audio_next))
            audio_anchor_pts = audio_pts
            audio_anchor_ts = audio_next
            if recovered:
                rebuffering = False
            output_leads.append(audio_next - wall)

    audio_ts = [ts for _, ts in audio_outputs]
    video_ts = [ts for _, ts in video_outputs]
    assert all(a < b for a, b in zip(audio_ts, audio_ts[1:])), "audio timestamp reversal"
    assert all(a <= b for a, b in zip(video_ts, video_ts[1:])), "video timestamp reversal"
    assert min(output_leads) >= MIN_OUTPUT_LEAD, "late audio reached OBS"
    assert max(output_leads) <= MAX_OUTPUT_LEAD, "audio scheduled too far ahead"
    assert buffer.max_seen <= HARD_CAP + AUDIO_DUR, "latency ceiling exceeded"

    if adaptive:
        assert min(ratios) >= 0.98
        assert max(ratios) <= 1.02
    else:
        assert set(ratios) == {1.0}, "original-speed mode changed playback rate"
        assert output_frame_counts == {AUDIO_FRAMES}, "original-speed mode resampled audio"

    max_sync = 0.0
    if not outage:
        stable_video_gaps = [
            current[1] - previous[1]
            for previous, current in zip(video_outputs, video_outputs[1:])
            if current[0] >= 20 * NS
        ]
        assert min(stable_video_gaps) > video_dur // 5
        assert max(stable_video_gaps) < video_dur * 4
        assert underruns == 0, f"avoidable stable-feed underruns: {underruns}"

        sync_errors = []
        for pts, video_ts_value in video_outputs:
            if pts < 20 * NS or pts > (duration_s - 2) * NS:
                continue
            audio_ts_value = interpolate(audio_outputs, pts)
            if audio_ts_value is not None:
                sync_errors.append((video_ts_value - audio_ts_value) / MS)
        assert sync_errors
        max_sync = max(abs(error) for error in sync_errors)
        assert max_sync < 40.0, f"A/V sync exceeded 40 ms: {max_sync:.2f} ms"
    else:
        assert underruns >= 1, "outage did not empty the queue"
        assert recoveries == underruns, "recovery cycle was incomplete"
        assert resyncs >= recoveries, "recovery did not start a new timestamp epoch"
        outage_end = (outage[0] + outage[1] + 2) * NS
        sync_errors = []
        for pts, video_ts_value in video_outputs:
            if pts < outage_end or pts > (duration_s - 2) * NS:
                continue
            audio_ts_value = interpolate(audio_outputs, pts)
            if audio_ts_value is not None:
                sync_errors.append(
                    (pts, (video_ts_value - audio_ts_value) / MS)
                )
        assert sync_errors
        worst_pts, worst_error = max(sync_errors, key=lambda item: abs(item[1]))
        max_sync = abs(worst_error)
        assert max_sync < 40.0, (
            "post-recovery A/V sync exceeded 40 ms: "
            f"{max_sync:.2f} ms at {worst_pts / NS:.2f}s "
            f"(underruns={underruns}, resyncs={resyncs})"
        )

    return {
        "max_sync_ms": max_sync,
        "max_buffer_ms": buffer.max_seen / MS,
        "clock_rate": clock.rate,
        "underruns": float(underruns),
        "recoveries": float(recoveries),
        "resyncs": float(resyncs),
    }


def test_two_hour_timeline() -> None:
    """Scheduler jitter must not create a late timestamp during a long run."""
    rng = random.Random(20260826)
    wall = 0
    next_ts = 0
    for index in range(int(2 * 60 * 60 * NS / AUDIO_DUR)):
        wall = max(wall, index * AUDIO_DUR + rng.randint(-2 * MS, 2 * MS))
        if index == 0:
            next_ts = wall + OUTPUT_LEAD
        else:
            candidate = next_ts + AUDIO_DUR
            if not wall + MIN_OUTPUT_LEAD <= candidate <= wall + MAX_OUTPUT_LEAD:
                candidate = wall + OUTPUT_LEAD
            next_ts = candidate
        assert next_ts >= wall + MIN_OUTPUT_LEAD


def main() -> int:
    scenarios = [
        (1.00, 30, 0.0, False),
        (1.00, 60, 5.0, False),
        (0.99, 60, 5.0, True),
        (1.01, 30, 10.0, True),
    ]
    for rate, fps, jitter, adaptive in scenarios:
        metrics = simulate(rate, fps, jitter, adaptive=adaptive)
        print(
            f"rate={rate:.2f} fps={fps} jitter={jitter:.0f}ms "
            f"mode={'adaptive' if adaptive else 'original'} "
            f"sync={metrics['max_sync_ms']:.1f}ms "
            f"buffer={metrics['max_buffer_ms']:.0f}ms"
        )

    recovery = simulate(
        1.0,
        30,
        5.0,
        adaptive=False,
        duration_s=60,
        outage=(25, 4),
    )
    print(
        "four-second outage "
        f"underruns={recovery['underruns']:.0f} "
        f"recoveries={recovery['recoveries']:.0f} "
        f"resyncs={recovery['resyncs']:.0f}"
    )
    test_two_hour_timeline()
    print("two-hour original-speed timeline: stable")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"timing acceptance failed: {error}", file=sys.stderr)
        raise SystemExit(1)
