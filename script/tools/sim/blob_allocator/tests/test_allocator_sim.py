# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Unit tests and optional integration tests against the real C++ allocator."""

import contextlib
import csv
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import allocator_sim as sim

KiB = 1024


class PythonTests(unittest.TestCase):
    def config(self, **values):
        config = {
            'sizes': [4096, 8192],
            'weights': [0.25, 0.75],
            'seed': 42,
            'mode': '4k-align',
            'min_water_level': 80,
            'max_water_level': 90,
            'sample_every': 100,
            'steps': 1000,
            'capacity': 64 * KiB,
        }
        config.update(values)
        return config

    def test_parser_and_distribution(self):
        self.assertEqual(sim.size_value('2KiB'), 2048)
        self.assertEqual(sim.distribution('4KiB,8KiB')[1], [0.5, 0.5])
        self.assertEqual(sim.distribution('4KiB:25%,8KiB:75%')[1], [0.25, 0.75])
        for text in ('0', '-1', '1MB', str(1 << 64)):
            with self.assertRaises(ValueError):
                sim.size_value(text)
        for text in ('4KiB:25%,8KiB', '4KiB,4096', '4KiB:0%,8KiB:100%', '4KiB:50%'):
            with self.assertRaises(ValueError):
                sim.distribution(text)

    def test_workloads(self):
        self.assertEqual(list(sim.operations(self.config())), list(sim.operations(self.config())))
        self.assertTrue(all(isinstance(op, sim.Alloc) for op in sim.operations(self.config())))
        for low, high in ((-1, 90), (90, 90), (91, 90), (80, 101), (80.5, 90)):
            with (
                patch.object(
                    sim,
                    'build_native',
                    side_effect=AssertionError('Invalid config built code'),
                ),
                self.assertRaises(ValueError),
            ):
                sim.run_simulation(self.config(min_water_level=low, max_water_level=high))

    def test_histogram_and_percentages(self):
        buckets, exact = sim.histogram(
            [(index * 100000, size * KiB) for index, size in enumerate([4, 8, 12, 16, 16, 64])]
        )
        self.assertEqual(sum(bucket['count'] for bucket in buckets), 6)
        self.assertEqual(sum(bucket['bytes'] for bucket in buckets), 120 * KiB)
        self.assertEqual(buckets[2]['count'], 3)
        self.assertEqual(buckets[2]['bytes'], 44 * KiB)
        self.assertEqual(exact[16 * KiB], 2)
        self.assertEqual(sim.percentage(6, 16), 37.5)
        self.assertEqual(sim.percentage(16, 32), 50)
        self.assertIsNone(sim.percentage(0, 0))

    def test_help_never_builds(self):
        with patch.object(sim, 'build_native', side_effect=AssertionError('Help compiled code')):
            with (
                contextlib.redirect_stdout(io.StringIO()) as output,
                self.assertRaises(SystemExit) as error,
            ):
                sim.main(['--help'])
            self.assertEqual(error.exception.code, 0)
            for phrase in (
                'Start empty',
                'at or above',
                'without retrying',
                'max-value-align',
            ):
                self.assertIn(phrase, output.getvalue())

    def test_mode_choices_and_removed_flags(self):
        self.assertEqual(sim.parser().parse_args([]).mode, '4k-align')
        selected = sim.parser().parse_args(['--mode', 'max-value-align'])
        self.assertEqual(selected.mode, 'max-value-align')
        for flag in ('--compare', '--workload', '--alloc-probability'):
            with (
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                sim.parser().parse_args([flag])
        self.assertEqual(sim.parser().parse_args([]).min_water_level, 80)
        self.assertEqual(sim.parser().parse_args([]).max_water_level, 90)
        for invalid in ('normal', 'compare', 'off', 'on', 'max-value-alignment'):
            with (
                self.subTest(mode=invalid),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                sim.parser().parse_args(['--mode', invalid])

    def test_build_failure_aborts(self):
        with patch.object(sim, 'build_native', side_effect=RuntimeError('Build failed')):
            with (
                contextlib.redirect_stderr(io.StringIO()),
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                self.assertEqual(sim.main([]), 1)
            self.assertEqual(output.getvalue(), '')

    def test_json_header_override_and_cache_reset(self):
        for override in (None, '/custom/json/include'):
            with self.subTest(override=override), patch.dict(os.environ):
                os.environ.pop('BLOB_SIM_JSON_HEADERS', None)
                if override is not None:
                    os.environ['BLOB_SIM_JSON_HEADERS'] = override
                with (
                    patch.object(sim.shutil, 'which', return_value='/usr/bin/cmake'),
                    patch.object(sim.importlib.util, 'find_spec', return_value=object()),
                    patch.object(
                        sim.subprocess,
                        'run',
                        side_effect=RuntimeError('Captured configure'),
                    ) as configure,
                ):
                    with self.assertRaisesRegex(RuntimeError, 'Captured configure'):
                        sim.build_native()
                    self.assertIn(f'-DJSON_HEADERS={override or ""}', configure.call_args.args[0])

    def test_optional_reports(self):
        report = {
            'configuration': {},
            'modes': {
                '4k-align': {
                    'samples': [
                        {
                            'operation_index': 0,
                            'internal_percent': None,
                            'external_percent': 50.0,
                        }
                    ]
                }
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            json_path, csv_path = root / 'report.json', root / 'report.csv'
            sim.write_reports(report, None, None)
            self.assertEqual(list(root.iterdir()), [])
            sim.write_reports(report, json_path, csv_path)
            self.assertEqual(json.loads(json_path.read_text()), report)
            with csv_path.open(newline='', encoding='utf-8') as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row['internal_percent'], '')
            self.assertEqual(row['external_percent'], '50.0')
            with self.assertRaises(ValueError):
                sim.write_reports(report, json_path, json_path)


@unittest.skipUnless(
    os.environ.get('BLOB_SIM_NATIVE_TESTS') == '1',
    'Set BLOB_SIM_NATIVE_TESTS=1 for native tests.',
)
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = sim.build_native()

    def test_internal_and_coalescing(self):
        allocator = self.native.Allocator(64 * KiB)
        handle = allocator.alloc(4097)
        self.assertEqual(allocator.snapshot()['internal_bytes'], 4095)
        allocator.free(handle)
        self.assertEqual(allocator.snapshot()['ranges'], [(0, 64 * KiB)])
        with self.assertRaises(ValueError):
            allocator.free(handle)
        for invalid in (0, (1 << 64) - 1):
            with self.assertRaises(ValueError):
                allocator.alloc(invalid)

    def test_external_fragmentation(self):
        allocator = self.native.Allocator(64 * KiB)
        blocks = [allocator.alloc(16 * KiB) for _ in range(4)]
        allocator.free(blocks[0])
        allocator.free(blocks[2])
        self.assertIsNone(allocator.alloc(24 * KiB))
        state = allocator.snapshot()
        self.assertEqual(state['largest_free'], 16 * KiB)
        self.assertEqual(sum(size for _, size in state['ranges']), 32 * KiB)
        allocator.free(blocks[1])
        allocator.free(blocks[3])
        self.assertEqual(allocator.snapshot()['ranges'], [(0, 64 * KiB)])

    def test_runner_fragmentation_percentage(self):
        sequence = [sim.Alloc(index, 16 * KiB) for index in range(4)]
        sequence.extend([sim.Free(0), sim.Free(2), sim.Alloc(4, 24 * KiB)])
        result = sim.run_workload(iter(sequence), 64 * KiB)['4k-align']
        self.assertEqual(result['final']['external_percent'], 50.0)
        self.assertEqual(result['counters']['fragmentation_failures'], 1)
        self.assertEqual(result['final']['internal_percent'], 0.0)
        result = sim.run_workload(iter([sim.Alloc(0, 4097)]), 64 * KiB)['4k-align']
        self.assertAlmostEqual(result['final']['internal_percent'], 100 * 4095 / 8192)

    def test_max_value_alignment(self):
        allocator = self.native.Allocator(64 * KiB, True)
        handle = allocator.alloc(16 * KiB)
        allocator.free(handle)
        small = allocator.alloc(4 * KiB)
        self.assertEqual(allocator.blob_info(small)['reserved'], 16 * KiB)
        self.assertEqual(allocator.snapshot()['internal_bytes'], 12 * KiB)

    def test_runner_and_abort(self):
        sequence = [sim.Alloc(0, 8192), sim.Alloc(1, 4096), sim.Free(0)]
        report = sim.run_workload(iter(sequence), 64 * KiB)
        self.assertEqual(report['4k-align']['final']['reserved'], 4096)
        report = sim.run_workload(iter(sequence), 64 * KiB, mode='max-value-align')
        self.assertEqual(report['max-value-align']['final']['reserved'], 8192)
        with self.assertRaisesRegex(RuntimeError, 'Operation 3'):
            sim.run_workload(iter([sim.Alloc(0, 8192), sim.Free(0), sim.Free(0)]), 64 * KiB)
        report = sim.run_workload(iter([sim.Alloc(0, 128 * KiB), sim.Free(0)]), 64 * KiB)
        self.assertEqual(report['4k-align']['counters']['skipped_releases'], 1)
        with self.assertRaisesRegex(RuntimeError, 'Operation 3'):
            sim.run_workload(iter([sim.Alloc(0, 128 * KiB), sim.Free(0), sim.Free(0)]), 64 * KiB)

    def simulation(self, **overrides):
        config = PythonTests().config(
            sizes=[4 * KiB],
            weights=[1],
            steps=4,
            capacity=16 * KiB,
            min_water_level=50,
            max_water_level=100,
        )
        config.update(overrides)
        return sim.run_simulation(config)[config['mode']]

    def test_eviction_boundaries_and_final_attempt(self):
        for mode in ('4k-align', 'max-value-align'):
            result = self.simulation(mode=mode)
            self.assertEqual(result['counters']['attempts'], 4)
            self.assertEqual(result['counters']['evictions'], 2)
            self.assertEqual(result['counters']['releases'], 2)
            self.assertEqual(result['counters']['eviction_cycles'], 1)
            self.assertEqual(result['counters']['evicted_reserved_bytes'], 8 * KiB)
            self.assertEqual(result['final']['reserved'], 8 * KiB)
            self.assertEqual(result['final']['allocation_index'], 4)
            self.assertEqual(result['final']['operation_index'], 6)
            self.assertEqual(
                [x['occupancy_percent'] for x in result['samples'] if x['phase'].startswith('eviction-')],
                [100, 50],
            )
        result = self.simulation(min_water_level=0, max_water_level=90)
        self.assertEqual(result['final']['reserved'], 0)
        result = self.simulation(capacity=16 * KiB + 1, min_water_level=50, max_water_level=90)
        self.assertEqual(result['final']['reserved'], 8 * KiB)
        result = self.simulation(min_water_level=40, max_water_level=70, steps=3)
        self.assertEqual(result['final']['reserved'], 4 * KiB)

    def test_failure_does_not_evict(self):
        sequence = [
            sim.Alloc(0, 12 * KiB),
            sim.Alloc(1, 8 * KiB),
            sim.Alloc(2, 8 * KiB),
        ]
        with patch.object(sim, 'operations', return_value=iter(sequence)):
            result = self.simulation(steps=3)
        self.assertEqual(result['counters']['capacity_failures'], 2)
        self.assertEqual(result['counters']['evictions'], 0)
        self.assertEqual(result['counters']['attempts'], 3)
        self.assertEqual(result['counters']['skipped_releases'], 0)

    def test_eviction_reproducibility_and_padding(self):
        options = {'sizes': [4 * KiB, 8 * KiB], 'weights': [0.5, 0.5], 'steps': 100}
        self.assertEqual(self.simulation(**options), self.simulation(**options))
        a = self.simulation(**options)
        b = self.simulation(**options, min_water_level=0, mode='max-value-align')
        self.assertEqual(a['counters']['attempts'], b['counters']['attempts'])
        self.assertEqual(
            {k: v['attempts'] for k, v in a['per_size'].items()},
            {k: v['attempts'] for k, v in b['per_size'].items()},
        )
        sequence = [sim.Alloc(0, 8 * KiB), sim.Alloc(1, 1)]
        with patch.object(sim, 'operations', side_effect=lambda _: iter(sequence)):
            normal = self.simulation(steps=2)
            padded = self.simulation(steps=2, mode='max-value-align')
        self.assertEqual(normal['counters']['evictions'], 0)
        self.assertEqual(padded['counters']['eviction_cycles'], 1)

    def test_simulation_abort_and_reports(self):
        original = sim.Measurement.apply

        def fail_release(measurement, operation):
            if isinstance(operation, sim.Free):
                raise RuntimeError('Native release failed.')  # noqa: TRY004 - simulate a native release failure
            return original(measurement, operation)

        with (
            patch.object(sim.Measurement, 'apply', fail_release),
            self.assertRaisesRegex(RuntimeError, 'Allocation attempt 4, Operation 5: Native release failed'),
        ):
            self.simulation()
        result = self.simulation()
        report = {'configuration': {}, 'modes': {'4k-align': result}}
        with tempfile.TemporaryDirectory() as directory:
            json_path, csv_path = Path(directory) / 'out.json', Path(directory) / 'out.csv'
            sim.write_reports(report, json_path, csv_path)
            self.assertEqual(json.loads(json_path.read_text())['modes']['4k-align']['final']['occupancy_percent'], 50)
            with csv_path.open(encoding='utf-8', newline='') as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(rows[-1]['allocation_index'], '4')
            self.assertEqual(rows[-1]['evictions'], '2')


if __name__ == '__main__':
    unittest.main()
