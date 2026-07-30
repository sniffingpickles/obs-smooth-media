#!/usr/bin/env python3
"""Regression tests for the long-soak evidence evaluator."""

from __future__ import annotations

import csv
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("analyze-soak.py")
SPEC = importlib.util.spec_from_file_location("analyze_soak", MODULE_PATH)
assert SPEC and SPEC.loader
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)


def arguments(**overrides: object) -> SimpleNamespace:
    values: dict[str, object] = {
        "max_private_growth_mib": 16.0,
        "max_handle_growth": 4,
        "max_thread_growth": 2,
        "min_duration_seconds": 8.0,
        "max_sample_gap_seconds": 2.0,
        "max_audio_stall_seconds": 2.0,
        "max_video_stall_seconds": 2.0,
        "max_counter_resets": 0,
        "max_reconnect_samples": 0,
        "final_window_seconds": 4.0,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


class AnalyzeSoakTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_process(self, *, dead_at: int | None = None) -> Path:
        path = self.root / "process.csv"
        fields = [
            "Utc",
            "Alive",
            "ProcessId",
            "WorkingSet",
            "Private",
            "Handles",
            "Threads",
            "CpuSeconds",
        ]
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            for index in range(10):
                alive = index != dead_at
                writer.writerow(
                    {
                        "Utc": (
                            f"2026-07-30T00:00:0{index}.1234567Z"
                        ),
                        "Alive": str(alive),
                        "ProcessId": "42" if alive else "",
                        "WorkingSet": 1000 + index if alive else "",
                        "Private": 2000 + index if alive else "",
                        "Handles": 20 if alive else "",
                        "Threads": 5 if alive else "",
                        "CpuSeconds": index if alive else "",
                    }
                )
        return path

    def write_source(
        self,
        *,
        stalled_after: int | None = None,
        reset_at: int | None = None,
    ) -> Path:
        path = self.root / "source.jsonl"
        audio = 0
        video = 0
        with path.open("w", encoding="utf-8") as handle:
            for index in range(10):
                if reset_at == index:
                    audio = 0
                    video = 0
                if stalled_after is None or index < stalled_after:
                    audio += 2
                    video += 1
                item = {
                    "utc": f"2026-07-30T00:00:0{index}.1234567Z",
                    "state": "playing",
                    "active": True,
                    "reconnecting": False,
                    "audioFramesOut": audio,
                    "videoFramesOut": video,
                    "avOffsetMs": -180,
                }
                handle.write(json.dumps(item) + "\n")
        return path

    def test_healthy_soak_passes_and_uses_disjoint_tails(self) -> None:
        process, process_errors = ANALYZER.analyze_process(
            self.write_process(), arguments()
        )
        source, source_errors = ANALYZER.analyze_source(
            self.write_source(), arguments()
        )
        self.assertEqual(process_errors, [])
        self.assertEqual(source_errors, [])
        self.assertEqual(process["pids"], [42])
        self.assertEqual(process["privateBytes"]["growth"], 5.0)
        self.assertGreater(source["finalWindowVideoDelta"], 0)

    def test_dead_process_is_a_failure(self) -> None:
        _, errors = ANALYZER.analyze_process(
            self.write_process(dead_at=5), arguments()
        )
        self.assertTrue(any("OBS was absent" in error for error in errors))

    def test_stale_final_counters_are_a_failure(self) -> None:
        _, errors = ANALYZER.analyze_source(
            self.write_source(stalled_after=4), arguments()
        )
        self.assertTrue(any("video stalled" in error for error in errors))
        self.assertTrue(any("audio stalled" in error for error in errors))
        self.assertTrue(any("final window" in error for error in errors))

    def test_counter_reset_requires_an_explicit_allowance(self) -> None:
        path = self.write_source(reset_at=5)
        _, errors = ANALYZER.analyze_source(path, arguments())
        self.assertTrue(any("counters reset" in error for error in errors))
        _, allowed_errors = ANALYZER.analyze_source(
            path, arguments(max_counter_resets=1)
        )
        self.assertEqual(allowed_errors, [])

    def test_malformed_evidence_returns_machine_readable_failure(self) -> None:
        process = self.write_process()
        source = self.root / "malformed.jsonl"
        source.write_text('{"utc":"not-a-time"}\n{}\n', encoding="utf-8")
        result = subprocess.run(
            [
                sys.executable,
                str(MODULE_PATH),
                "--process-csv",
                str(process),
                "--source-jsonl",
                str(source),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 1)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["success"])
        self.assertTrue(
            any(
                "unable to analyze malformed.jsonl" in error
                for error in payload["errors"]
            )
        )


if __name__ == "__main__":
    unittest.main()
