# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Build and native allocator regressions. No service or NPU is required."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))
import allocator_sim as sim

ALIGNMENT = 4096
CAPACITY = 1024 * 1024


class DriverTests(unittest.TestCase):
    def test_import_does_not_build(self):
        spec = importlib.util.spec_from_file_location("driver_import_check", TOOL / "allocator_sim.py")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        try:
            with patch("subprocess.run", side_effect=AssertionError("Import started a build")):
                spec.loader.exec_module(module)
        finally:
            sys.modules.pop(spec.name, None)

    def test_help_does_not_need_build_tools(self):
        with tempfile.TemporaryDirectory() as empty_path:
            environment = dict(os.environ, PATH=empty_path)
            result = subprocess.run(
                [sys.executable, str(TOOL / "allocator_sim.py"), "--help"],
                env=environment, capture_output=True, text=True, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--mode", result.stdout)
        self.assertNotIn("CMake", result.stderr)

    def test_invalid_configuration_does_not_build(self):
        args = sim.argparse.Namespace(capacity="1MiB", block_sizes="4KiB", steps=0,
                                      sample_every=10, reads_per_allocation=0)
        with patch.object(sim, "_load_native_module", side_effect=AssertionError("Invalid run started a build")):
            with self.assertRaises(ValueError):
                sim.run_simulation(args)


class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = sim._load_native_module()

    def test_native_failure_and_concurrency(self):
        build = Path(self.native.__file__).resolve().parents[1]
        subprocess.run(["cmake", "--build", str(build), "--target", "slab_allocator_safety_test",
                        "--parallel", "2"], check=True)
        for slab_mode in ("0", "1"):
            env = dict(os.environ, MMC_ENABLE_SLAB_ALLOCATOR=slab_mode)
            subprocess.run([str(build / "slab_allocator_safety_test")], check=True, timeout=60, env=env)

    def assert_accounting(self, allocator, requested):
        snapshot = allocator.snapshot()
        self.assertEqual(snapshot["requested"], requested)
        self.assertEqual(snapshot["reserved"] + snapshot["free"], CAPACITY)
        self.assertEqual(snapshot["requested"] + snapshot["internal_fragmentation"], snapshot["reserved"])
        if "slab_stats" in snapshot:
            stats = snapshot["slab_stats"]
            free = stats["free_full_frame_bytes"] + stats["free_tail_frame_bytes"]
            free += sum(pool["free_bytes"] for pool in stats["pools"])
        else:
            free = sum(size for _, size in snapshot["free_ranges"])
        self.assertEqual(free, snapshot["free"])

    def test_all_modes_allocate_release_and_reuse(self):
        for mode in ("4k-align", "max-value-align", "slab"):
            with self.subTest(mode=mode):
                allocator = self.native.NativeAllocatorSim(CAPACITY, mode)
                first = allocator.alloc(ALIGNMENT + 1)
                second = allocator.alloc(1)
                self.assertIsNotNone(first)
                self.assertIsNotNone(second)
                self.assert_accounting(allocator, ALIGNMENT + 2)
                allocator.release(first[0])
                self.assert_accounting(allocator, 1)
                allocator.release(second[0])
                self.assert_accounting(allocator, 0)
                self.assertEqual(allocator.snapshot()["free"], CAPACITY)
                self.assertIsNotNone(allocator.alloc(ALIGNMENT + 1))

    def test_slab_uses_exact_classes(self):
        allocator = self.native.NativeAllocatorSim(CAPACITY, "slab")
        first = allocator.alloc(10 * ALIGNMENT)
        second = allocator.alloc(9 * ALIGNMENT)
        self.assertEqual(first[2], 10 * ALIGNMENT)
        self.assertEqual(second[2], 9 * ALIGNMENT)
        self.assertEqual(len(allocator.slab_free_stats()["pools"]), 2)
        self.assert_accounting(allocator, 19 * ALIGNMENT)

    def test_can_alloc_does_not_create_classes(self):
        allocator = self.native.NativeAllocatorSim(CAPACITY, "slab")
        for size in range(1, 300):
            allocator.can_alloc(size * ALIGNMENT)
        self.assertEqual(allocator.slab_free_stats()["pools"], [])
        self.assertIsNotNone(allocator.alloc(ALIGNMENT))

    def test_slab_exhaustion_and_frame_return(self):
        allocator = self.native.NativeAllocatorSim(CAPACITY, "slab")
        handles = []
        while True:
            block = allocator.alloc(ALIGNMENT)
            if block is None:
                break
            handles.append(block[0])
        self.assertFalse(allocator.can_alloc(ALIGNMENT))
        self.assert_accounting(allocator, CAPACITY)
        for handle in handles:
            allocator.release(handle)
        self.assert_accounting(allocator, 0)
        self.assertTrue(allocator.can_alloc(2 * ALIGNMENT))
        self.assertIsNotNone(allocator.alloc(2 * ALIGNMENT))

    def test_class_limit_never_falls_back_to_larger_slot(self):
        capacity = 64 * CAPACITY
        allocator = self.native.NativeAllocatorSim(capacity, "slab")
        for index in range(1, 257):
            block = allocator.alloc(2 * index * ALIGNMENT)
            self.assertIsNotNone(block)
            allocator.release(block[0])
        self.assertEqual(len(allocator.slab_free_stats()["pools"]), 256)
        self.assertTrue(allocator.can_alloc(ALIGNMENT))
        reclaimed = allocator.alloc(ALIGNMENT)
        self.assertIsNotNone(reclaimed)
        self.assertEqual(reclaimed[2], ALIGNMENT)
        self.assertEqual(len(allocator.slab_free_stats()["pools"]), 256)
        allocator.release(reclaimed[0])

        # Gdy wszystkie 256 klas aktywnie trzymają co najmniej jedną ramkę, limit jest twardy
        # (bez fallbacku do większego slotu), a po zwolnieniu jednej klasy można ją odzyskać.
        frame_size = 1536 * 1024 * 1024
        active_alloc = self.native.NativeAllocatorSim(260 * frame_size, "slab")
        handles = []
        for index in range(1, 257):
            block = active_alloc.alloc((128 * 1024 + index) * ALIGNMENT)
            self.assertIsNotNone(block)
            handles.append(block[0])
        self.assertEqual(len(active_alloc.slab_free_stats()["pools"]), 256)
        self.assertFalse(active_alloc.can_alloc(ALIGNMENT))
        self.assertIsNone(active_alloc.alloc(ALIGNMENT))

        active_alloc.release(handles[0])
        self.assertTrue(active_alloc.can_alloc(ALIGNMENT))
        new_block = active_alloc.alloc(ALIGNMENT)
        self.assertIsNotNone(new_block)
        self.assertEqual(new_block[2], ALIGNMENT)
        self.assertEqual(len(active_alloc.slab_free_stats()["pools"]), 256)

    def test_invalid_allocations_and_double_release(self):
        for mode in ("4k-align", "max-value-align", "slab"):
            with self.subTest(mode=mode):
                allocator = self.native.NativeAllocatorSim(CAPACITY, mode)
                for size in (0, (1 << 64) - 1):
                    with self.assertRaises(ValueError):
                        allocator.alloc(size)
                first = allocator.alloc(1)
                allocator.release(first[0])
                with self.assertRaises(ValueError):
                    allocator.release(first[0])
                self.assert_accounting(allocator, 0)


if __name__ == "__main__":
    unittest.main()
