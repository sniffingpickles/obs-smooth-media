#!/usr/bin/env python3
"""Evaluate long OBS soak checkpoints without trusting one start/end sample."""

from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path


def parse_utc(value: str) -> datetime:
    value = value.strip().replace("Z", "+00:00")
    # PowerShell's round-trip format can contain seven fractional digits;
    # datetime.fromisoformat accepts microseconds, so trim only the excess.
    value = re.sub(r"(\.\d{6})\d+(?=[+-]\d\d:\d\d$)", r"\1", value)
    return datetime.fromisoformat(value)


def as_bool(value: str | bool | None) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in {"1", "true", "yes"}


def tail_median(values: list[int], first: bool) -> float:
    # Use 10% tails with at least five samples on a real soak, but never let
    # the two tails overlap on a short qualification run.
    count = min(max(5, len(values) // 10), max(1, len(values) // 2))
    window = values[:count] if first else values[-count:]
    return float(statistics.median(window))


def slope_per_hour(times: list[datetime], values: list[int]) -> float:
    if len(values) < 2:
        return 0.0
    origin = times[0]
    x = [(time - origin).total_seconds() / 3600.0 for time in times]
    mean_x = statistics.fmean(x)
    mean_y = statistics.fmean(values)
    denominator = sum((item - mean_x) ** 2 for item in x)
    if denominator == 0:
        return 0.0
    return sum(
        (item - mean_x) * (value - mean_y)
        for item, value in zip(x, values, strict=True)
    ) / denominator


def analyze_process(path: Path, args: argparse.Namespace) -> tuple[dict, list[str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    errors: list[str] = []
    if len(rows) < 2:
        return {"samples": len(rows)}, ["process CSV has fewer than two samples"]

    dead = [row for row in rows if "Alive" in row and not as_bool(row["Alive"])]
    if dead:
        errors.append(f"OBS was absent in {len(dead)} process sample(s)")
    live = [
        row
        for row in rows
        if "Alive" not in row or as_bool(row.get("Alive"))
    ]
    if len(live) < 2:
        return {"samples": len(rows), "liveSamples": len(live)}, errors + [
            "process CSV has fewer than two live samples"
        ]

    times = [parse_utc(row["Utc"]) for row in live]
    non_increasing = sum(
        right <= left for left, right in zip(times, times[1:])
    )
    if non_increasing:
        errors.append(
            f"process timestamps were non-increasing {non_increasing} time(s)"
        )
    gaps = [
        (right - left).total_seconds()
        for left, right in zip(times, times[1:])
    ]
    maximum_gap = max(gaps)
    if (
        args.max_sample_gap_seconds > 0
        and maximum_gap > args.max_sample_gap_seconds
    ):
        errors.append(
            f"process sample gap reached {maximum_gap:.1f}s "
            f"(limit {args.max_sample_gap_seconds:.1f}s)"
        )
    duration = (times[-1] - times[0]).total_seconds()
    if duration < args.min_duration_seconds:
        errors.append(
            f"process duration was {duration:.1f}s "
            f"(minimum {args.min_duration_seconds:.1f}s)"
        )
    private = [int(row["Private"]) for row in live]
    working_set = [int(row["WorkingSet"]) for row in live]
    handles = [int(row["Handles"]) for row in live]
    threads = [int(row["Threads"]) for row in live]
    pids = {
        int(row["ProcessId"])
        for row in live
        if row.get("ProcessId") not in {None, ""}
    }
    if len(pids) > 1:
        errors.append(f"OBS PID changed during soak: {sorted(pids)}")

    private_growth = tail_median(private, False) - tail_median(private, True)
    handle_growth = tail_median(handles, False) - tail_median(handles, True)
    thread_growth = tail_median(threads, False) - tail_median(threads, True)
    private_limit = args.max_private_growth_mib * 1024 * 1024
    if private_growth > private_limit:
        errors.append(
            f"private-byte tail median grew {private_growth / 1048576:.1f} MiB "
            f"(limit {args.max_private_growth_mib:.1f} MiB)"
        )
    if handle_growth > args.max_handle_growth:
        errors.append(
            f"handle tail median grew {handle_growth:.0f} "
            f"(limit {args.max_handle_growth})"
        )
    if thread_growth > args.max_thread_growth:
        errors.append(
            f"thread tail median grew {thread_growth:.0f} "
            f"(limit {args.max_thread_growth})"
        )

    summary = {
        "samples": len(rows),
        "liveSamples": len(live),
        "durationSeconds": duration,
        "maximumSampleGapSeconds": maximum_gap,
        "pids": sorted(pids),
        "workingSetBytes": {
            "firstTailMedian": tail_median(working_set, True),
            "lastTailMedian": tail_median(working_set, False),
            "minimum": min(working_set),
            "maximum": max(working_set),
        },
        "privateBytes": {
            "firstTailMedian": tail_median(private, True),
            "lastTailMedian": tail_median(private, False),
            "growth": private_growth,
            "minimum": min(private),
            "maximum": max(private),
            "linearSlopeBytesPerHour": slope_per_hour(times, private),
        },
        "handles": {
            "firstTailMedian": tail_median(handles, True),
            "lastTailMedian": tail_median(handles, False),
            "growth": handle_growth,
            "minimum": min(handles),
            "maximum": max(handles),
        },
        "threads": {
            "firstTailMedian": tail_median(threads, True),
            "lastTailMedian": tail_median(threads, False),
            "growth": thread_growth,
            "minimum": min(threads),
            "maximum": max(threads),
        },
    }
    return summary, errors


def analyze_source(path: Path, args: argparse.Namespace) -> tuple[dict, list[str]]:
    observations = []
    with path.open(encoding="utf-8-sig") as handle:
        for number, line in enumerate(handle, 1):
            if line.strip():
                try:
                    observations.append(json.loads(line))
                except json.JSONDecodeError as error:
                    return {"samples": len(observations)}, [
                        f"{path.name}:{number}: invalid JSON: {error}"
                    ]
    if len(observations) < 2:
        return {"samples": len(observations)}, [
            f"{path.name} has fewer than two observations"
        ]

    errors: list[str] = []
    times = [parse_utc(item["utc"]) for item in observations]
    non_increasing = sum(
        right <= left for left, right in zip(times, times[1:])
    )
    if non_increasing:
        errors.append(
            f"{path.name} timestamps were non-increasing "
            f"{non_increasing} time(s)"
        )
    gaps = [
        (right - left).total_seconds()
        for left, right in zip(times, times[1:])
    ]
    maximum_gap = max(gaps)
    if (
        args.max_sample_gap_seconds > 0
        and maximum_gap > args.max_sample_gap_seconds
    ):
        errors.append(
            f"{path.name} sample gap reached {maximum_gap:.1f}s "
            f"(limit {args.max_sample_gap_seconds:.1f}s)"
        )
    duration = (times[-1] - times[0]).total_seconds()
    if duration < args.min_duration_seconds:
        errors.append(
            f"{path.name} duration was {duration:.1f}s "
            f"(minimum {args.min_duration_seconds:.1f}s)"
        )
    final = observations[-1]
    if final.get("state") != "playing" or not as_bool(final.get("active")):
        errors.append(
            f"{path.name} final state is {final.get('state')!r}, "
            f"active={final.get('active')!r}"
        )

    def counter_health(field: str) -> tuple[float, int]:
        maximum_stall = 0.0
        stall_start = times[0]
        previous = int(observations[0][field])
        resets = 0
        for time, item in zip(times[1:], observations[1:], strict=True):
            current = int(item[field])
            if current < previous:
                resets += 1
                stall_start = time
            elif current > previous:
                maximum_stall = max(
                    maximum_stall, (time - stall_start).total_seconds()
                )
                stall_start = time
            previous = current
        maximum_stall = max(
            maximum_stall, (times[-1] - stall_start).total_seconds()
        )
        return maximum_stall, resets

    max_video_stall, video_resets = counter_health("videoFramesOut")
    max_audio_stall, audio_resets = counter_health("audioFramesOut")
    reconnect_samples = 0
    for item in observations:
        if (
            item.get("state") != "playing"
            or not as_bool(item.get("active"))
            or as_bool(item.get("reconnecting"))
        ):
            reconnect_samples += 1
    if max_video_stall > args.max_video_stall_seconds:
        errors.append(
            f"{path.name} video stalled for {max_video_stall:.1f}s "
            f"(limit {args.max_video_stall_seconds:.1f}s)"
        )
    if max_audio_stall > args.max_audio_stall_seconds:
        errors.append(
            f"{path.name} audio stalled for {max_audio_stall:.1f}s "
            f"(limit {args.max_audio_stall_seconds:.1f}s)"
        )
    counter_resets = max(video_resets, audio_resets)
    if counter_resets > args.max_counter_resets:
        errors.append(
            f"{path.name} counters reset {counter_resets} time(s) "
            f"(limit {args.max_counter_resets})"
        )
    if reconnect_samples > args.max_reconnect_samples:
        errors.append(
            f"{path.name} had {reconnect_samples} reconnect/inactive sample(s) "
            f"(limit {args.max_reconnect_samples})"
        )

    final_window_start = times[-1].timestamp() - args.final_window_seconds
    final_window = [
        item
        for time, item in zip(times, observations, strict=True)
        if time.timestamp() >= final_window_start
    ]
    if len(final_window) < 2:
        errors.append(f"{path.name} has too few final-window samples")
        video_delta = 0
        audio_delta = 0
    else:
        video_delta = int(final_window[-1]["videoFramesOut"]) - int(
            final_window[0]["videoFramesOut"]
        )
        audio_delta = int(final_window[-1]["audioFramesOut"]) - int(
            final_window[0]["audioFramesOut"]
        )
        if video_delta <= 0:
            errors.append(f"{path.name} video did not advance in final window")
        if audio_delta <= 0:
            errors.append(f"{path.name} audio did not advance in final window")

    offsets = [int(item["avOffsetMs"]) for item in observations]
    summary = {
        "path": str(path),
        "samples": len(observations),
        "durationSeconds": duration,
        "maximumSampleGapSeconds": maximum_gap,
        "finalState": final.get("state"),
        "finalActive": as_bool(final.get("active")),
        "finalAudioFramesOut": int(final["audioFramesOut"]),
        "finalVideoFramesOut": int(final["videoFramesOut"]),
        "finalAvOffsetMs": int(final["avOffsetMs"]),
        "minimumAvOffsetMs": min(offsets),
        "maximumAvOffsetMs": max(offsets),
        "maximumAudioStallSeconds": max_audio_stall,
        "maximumVideoStallSeconds": max_video_stall,
        "reconnectSamples": reconnect_samples,
        "counterResets": counter_resets,
        "audioCounterResets": audio_resets,
        "videoCounterResets": video_resets,
        "finalWindowSeconds": args.final_window_seconds,
        "finalWindowAudioDelta": audio_delta,
        "finalWindowVideoDelta": video_delta,
    }
    return summary, errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--process-csv", required=True, type=Path)
    parser.add_argument(
        "--source-jsonl", required=True, action="append", type=Path
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--max-private-growth-mib", type=float, default=128.0)
    parser.add_argument("--max-handle-growth", type=int, default=64)
    parser.add_argument("--max-thread-growth", type=int, default=8)
    parser.add_argument("--min-duration-seconds", type=float, default=0.0)
    parser.add_argument("--max-sample-gap-seconds", type=float, default=0.0)
    parser.add_argument("--max-audio-stall-seconds", type=float, default=10.0)
    parser.add_argument("--max-video-stall-seconds", type=float, default=10.0)
    parser.add_argument("--max-counter-resets", type=int, default=0)
    parser.add_argument("--max-reconnect-samples", type=int, default=0)
    parser.add_argument("--final-window-seconds", type=float, default=60.0)
    args = parser.parse_args()

    errors: list[str] = []
    try:
        process, process_errors = analyze_process(args.process_csv, args)
        errors.extend(process_errors)
    except (OSError, KeyError, TypeError, ValueError) as error:
        process = {"path": str(args.process_csv)}
        errors.append(f"unable to analyze process CSV: {error}")
    sources = []
    for path in args.source_jsonl:
        try:
            source, source_errors = analyze_source(path, args)
        except (OSError, KeyError, TypeError, ValueError) as error:
            source = {"path": str(path)}
            source_errors = [f"unable to analyze {path.name}: {error}"]
        sources.append(source)
        errors.extend(source_errors)

    result = {
        "success": not errors,
        "process": process,
        "sources": sources,
        "errors": errors,
    }
    text = json.dumps(result, indent=2, sort_keys=True, allow_nan=False)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
