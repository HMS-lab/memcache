# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Unit and integration tests for allocator_sim.py."""

import argparse
import contextlib
import io
import json
import random
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))
import allocator_sim as sim

KiB = 1024
MiB = 1024 * 1024


def make_args(**overrides) -> argparse.Namespace:
    defaults = {
        "capacity": "2MiB",
        "block_sizes": "4KiB:50%,16KiB:50%",
        "steps": 200,
        "mode": "4k-align",
        "eviction_policy": "lru",
        "max_water_level": 90.0,
        "min_water_level": 80.0,
        "hot_block_percent": 20.0,
        "hot_read_percent": 80.0,
        "reads_per_allocation": 4,
        "sample_every": 100,
        "seed": 42,
        "workload": "water-level",
        "json_report_path": None,
    }
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


class SimulatorUnitTests(unittest.TestCase):
    def test_parse_and_format_sizes(self):
        self.assertEqual(sim.parse_size("2KiB"), 2048)
        self.assertEqual(sim.parse_size("4k"), 4096)
        self.assertEqual(sim.parse_size("1_024"), 1024)
        self.assertEqual(sim.parse_size("1.5MiB"), int(1.5 * MiB))
        self.assertEqual(sim.format_human(0), "0 B")
        self.assertEqual(sim.format_human(512), "512 B")
        self.assertEqual(sim.format_human(4 * KiB), "4.00 KiB")
        self.assertIn("4.00 KiB", sim.format_bytes(4 * KiB))
        for bad in ("", "abc", "10xyz", "1KiB2"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                sim.parse_size(bad)

    def test_parse_block_sizes(self):
        sizes, weights = sim.parse_block_sizes("4KiB,8KiB")
        self.assertEqual(sizes, [4 * KiB, 8 * KiB])
        self.assertEqual(weights, [50.0, 50.0])

        sizes, weights = sim.parse_block_sizes("4KiB:25%,16KiB:75%")
        self.assertEqual(sizes, [4 * KiB, 16 * KiB])
        self.assertAlmostEqual(weights[0], 25.0)
        self.assertAlmostEqual(weights[1], 75.0)

        for bad in ("", "0", "4KiB:0", "4KiB:-5%", "4KiB:nan"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                sim.parse_block_sizes(bad)

    def test_live_blocks_policies(self):
        # LRU: odczyt A przenosi A na koniec, więc ewikcja usuwa B
        lru = sim.LiveBlocks("lru", random.Random(1), random.Random(2))
        lru.add(sim.LiveBlock(handle=1, size=4096, reserved_size=4096, is_hot=True))
        lru.add(sim.LiveBlock(handle=2, size=4096, reserved_size=4096, is_hot=False))
        self.assertEqual(lru.read_one(prefer_hot=True), "hot")
        self.assertEqual(lru.pop_victim().handle, 2)
        self.assertEqual(lru.cold_count, 0)
        self.assertEqual(lru.hot_count, 1)

        # FIFO: odczyt A nie zmienia kolejności ewikcji, więc ewikcja usuwa A
        fifo = sim.LiveBlocks("fifo", random.Random(1), random.Random(2))
        fifo.add(sim.LiveBlock(handle=1, size=4096, reserved_size=4096, is_hot=True))
        fifo.add(sim.LiveBlock(handle=2, size=4096, reserved_size=4096, is_hot=False))
        self.assertEqual(fifo.read_one(prefer_hot=True), "hot")
        self.assertEqual(fifo.pop_victim().handle, 1)
        self.assertEqual(fifo.hot_count, 0)
        self.assertEqual(fifo.cold_count, 1)

        # Random: wyczerpanie puli i fallback na dostępny typ
        rnd = sim.LiveBlocks("random", random.Random(10), random.Random(20))
        self.assertIsNone(rnd.read_one(prefer_hot=True))
        rnd.add(sim.LiveBlock(handle=7, size=4096, reserved_size=4096, is_hot=False))
        self.assertEqual(rnd.read_one(prefer_hot=True), "cold")
        self.assertEqual(rnd.pop_victim().handle, 7)
        self.assertIsNone(rnd.pop_victim())

        # Fallback na dostępną pulę mimo nieaktualnych uchwytów po eksmisji
        for policy in ("random", "lru"):
            lb = sim.LiveBlocks(policy, random.Random(1), random.Random(2))
            lb.add(sim.LiveBlock(handle=10, size=4096, reserved_size=4096, is_hot=True))
            self.assertEqual(lb.pop_victim().handle, 10)
            lb.add(sim.LiveBlock(handle=11, size=4096, reserved_size=4096, is_hot=False))
            self.assertEqual(lb.read_one(prefer_hot=True), "cold")

    def test_histogram_and_slab_formatting(self):
        buckets = sim.build_histogram_buckets(64 * KiB)
        hist = sim.compute_histogram([(0, 4 * KiB), (8 * KiB, 8 * KiB)], buckets)
        self.assertEqual(hist[0][1:], (1, 4 * KiB))
        self.assertEqual(hist[1][1:], (1, 8 * KiB))
        self.assertEqual(sum(item[2] for item in hist), 12 * KiB)

        empty_text = sim.format_slab_summary(
            {
                "frame_size": 64 * KiB,
                "free_full_frames": 15,
                "free_full_frame_bytes": 15 * 64 * KiB,
                "free_tail_frame_bytes": 0,
                "pools": [],
            }
        )
        self.assertIn("(no active slab pools)", empty_text)

        filled_text = sim.format_slab_summary(
            {
                "frame_size": 64 * KiB,
                "free_full_frames": 1,
                "free_full_frame_bytes": 64 * KiB,
                "free_tail_frame_bytes": 4 * KiB,
                "pools": [
                    {
                        "slot_size": 8 * KiB,
                        "active_frames": 2,
                        "allocated_slots": 10,
                        "free_slots": 6,
                        "free_bytes": 48 * KiB,
                        "tail_waste_bytes": 0,
                    }
                ],
            }
        )
        self.assertIn("Free tail frame  : 1", filled_text)
        self.assertIn("8.00 KiB", filled_text)

    def test_invalid_configurations_never_build_native(self):
        bad_cases = [
            {"capacity": "0"},
            {"steps": 0},
            {"sample_every": 0},
            {"reads_per_allocation": -1},
            {"min_water_level": 90.0, "max_water_level": 90.0},
            {"min_water_level": 95.0, "max_water_level": 90.0},
            {"min_water_level": -1.0, "max_water_level": 90.0},
            {"hot_block_percent": 101.0},
            {"hot_read_percent": -1.0},
        ]
        for overrides in bad_cases:
            with (
                self.subTest(overrides=overrides),
                patch.object(sim, "_load_native_module", side_effect=AssertionError("Invalid run built native")),
                self.assertRaises(ValueError),
            ):
                sim.run_simulation(make_args(**overrides))


class SimulatorIntegrationTests(unittest.TestCase):
    def test_run_simulation_all_modes_and_reports(self):
        for mode in ("4k-align", "max-value-align", "slab"):
            with self.subTest(mode=mode), contextlib.redirect_stdout(io.StringIO()):
                report = sim.run_simulation(make_args(mode=mode, steps=250, capacity="1MiB"))
            self.assertEqual(report["counters"]["attempts"], 250)
            self.assertGreater(report["counters"]["successes"], 0)
            self.assertEqual(report["memory"]["capacity"], MiB)
            self.assertEqual(
                report["memory"]["reserved"] + report["memory"]["free"],
                report["memory"]["capacity"],
            )
            if mode == "slab":
                self.assertIn("slab_summary", report)
                self.assertNotIn("free_range_histogram", report)
            else:
                self.assertIn("free_range_histogram", report)
                self.assertNotIn("slab_summary", report)

    def test_same_seed_gives_identical_size_sequence_across_modes(self):
        # Przy 512KiB ramka slab ma 32KiB, więc żądania 128KiB zawodzą w trybie slab,
        # ale udają się w 4k-align. Liczba prób dla każdego rozmiaru musi pozostać identyczna.
        common = {
            "capacity": "512KiB",
            "block_sizes": "4KiB:50%,128KiB:50%",
            "steps": 120,
            "seed": 77,
            "eviction_policy": "random",
            "max_water_level": 80.0,
            "min_water_level": 50.0,
        }
        with contextlib.redirect_stdout(io.StringIO()):
            best_fit_report = sim.run_simulation(make_args(mode="4k-align", **common))
            slab_report = sim.run_simulation(make_args(mode="slab", **common))

        self.assertGreater(best_fit_report["size_stats"][1]["successes"], 0)
        self.assertEqual(slab_report["size_stats"][1]["successes"], 0)
        self.assertEqual(
            [s["attempts"] for s in best_fit_report["size_stats"]],
            [s["attempts"] for s in slab_report["size_stats"]],
        )
        self.assertEqual(best_fit_report["counters"]["attempts"], slab_report["counters"]["attempts"])

    def test_main_writes_json_report_and_handles_invalid_cli(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            json_path = Path(tmpdir) / "report.json"
            argv = [
                "allocator_sim.py",
                "--capacity",
                "1MiB",
                "--block-sizes",
                "4KiB:50%,8KiB:50%",
                "--mode",
                "slab",
                "--steps",
                "80",
                "--sample-every",
                "80",
                "--json-report-path",
                str(json_path),
            ]
            with patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()) as out:
                rc = sim.main()
            self.assertEqual(rc, 0)
            self.assertTrue(json_path.is_file())
            data = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual(data["configuration"]["mode"], "slab")
            self.assertIn("Slab Allocator Frame & Pool Summary:", out.getvalue())

        bad_argv = ["allocator_sim.py", "--capacity", "1MiB", "--block-sizes", "4KiB", "--steps", "0"]
        with (
            patch.object(sys, "argv", bad_argv),
            contextlib.redirect_stderr(io.StringIO()) as err,
        ):
            rc = sim.main()
        self.assertEqual(rc, 1)
        self.assertIn("Measurement aborted:", err.getvalue())


if __name__ == "__main__":
    unittest.main()
