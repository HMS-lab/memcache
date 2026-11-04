# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Unit tests and optional integration tests against the real C++ allocator."""

import contextlib
import csv
import io
import json
import os
import random
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

    def test_access_options_and_validation(self):
        defaults = sim.parser().parse_args([])
        self.assertEqual(defaults.eviction_policy, 'random')
        self.assertEqual(defaults.reads_per_allocation, 4)
        self.assertEqual(defaults.hot_block_percent, 20)
        self.assertEqual(defaults.hot_read_percent, 80)
        self.assertEqual(sim.parser().parse_args(['--eviction-policy', 'lru']).eviction_policy, 'lru')
        for key, value in (
            ('eviction_policy', 'fifo'),
            ('reads_per_allocation', -1),
            ('reads_per_allocation', 1.5),
            ('reads_per_allocation', True),
            ('hot_block_percent', -1),
            ('hot_block_percent', 101),
            ('hot_read_percent', 101),
            ('hot_read_percent', 0.5),
            ('hot_read_percent', False),
        ):
            with (
                self.subTest(key=key, value=value),
                patch.object(sim, 'build_native') as build,
                self.assertRaises(ValueError),
            ):
                sim.run_simulation(self.config(**{key: value}))
            build.assert_not_called()
        with (
            patch.object(sim, 'build_native') as build,
            contextlib.redirect_stderr(io.StringIO()) as error,
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(sim.main(['--reads-per-allocation', '-1']), 1)
        build.assert_not_called()
        self.assertIn('nonnegative integer', error.getvalue())
        self.assertEqual(output.getvalue(), '')

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
                '--steps',
                'at or above',
                'max-value-align',
                'least recently used',
                '--reads-per-allocation',
            ):
                self.assertIn(phrase, output.getvalue())

    def test_progress_percentages_and_redirected_output(self):
        output = io.StringIO()
        progress = sim.Progress(output)
        for completed in (0, 1, 2, 3, 100, 300):
            progress(completed, 300)
        progress.close()
        self.assertEqual(
            output.getvalue().splitlines(),
            [
                'Progress:   0% (0/300 allocation attempts)',
                'Progress:   1% (3/300 allocation attempts)',
                'Progress:  33% (100/300 allocation attempts)',
                'Progress: 100% (300/300 allocation attempts)',
            ],
        )
        self.assertNotIn('\r', output.getvalue())

    def test_terminal_progress_completion_and_abort(self):
        for completed in (1, 3):
            output = io.StringIO()
            with patch.object(output, 'isatty', return_value=True):
                progress = sim.Progress(output)
            progress(0, 3)
            self.assertFalse(output.getvalue().endswith('\n'))
            progress(completed, 3)
            progress.close()
            progress.close()
            self.assertEqual(output.getvalue().count('\r'), 2)
            self.assertEqual(output.getvalue().count('\n'), 1)
            self.assertTrue(output.getvalue().endswith('\n'))
            self.assertEqual('100%' in output.getvalue(), completed == 3)

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


class LiveBlocksTests(unittest.TestCase):
    def test_lru_promotion_and_invalid_access(self):
        live = sim.LiveBlocks()
        rng = random.Random(42)
        live.add(0, True)
        live.add(1, False)
        self.assertEqual(live.victim('lru', rng), 0)
        self.assertTrue(live.touch(0))
        live.touch(0)
        self.assertEqual(live.victim('lru', rng), 1)
        live.remove(1)
        self.assertEqual(live.victim('lru', rng), 0)
        for action in (live.touch, live.remove):
            with self.assertRaises(ValueError):
                action(1)
        with self.assertRaises(ValueError):
            live.add(0, False)
        self.assertEqual(live.snapshot({0: object()}), {'live_hot_blocks': 1, 'live_cold_blocks': 0})
        live.remove(0)
        with self.assertRaises(RuntimeError):
            live.victim('lru', rng)

    def test_hot_cold_selection_and_empty_groups(self):
        live = sim.LiveBlocks()
        rng = random.Random(42)
        self.assertIsNone(live.read_target(rng, 80))
        live.add(0, True)
        live.add(1, False)
        live.add(2, True)
        self.assertEqual({live.read_target(rng, 100) for _ in range(100)}, {0, 2})
        self.assertEqual({live.read_target(rng, 0) for _ in range(100)}, {1})
        live.remove(0)
        live.remove(2)
        self.assertEqual(live.read_target(rng, 100), 1)
        self.assertFalse(live.touch(1))
        live.remove(1)
        self.assertIsNone(live.read_target(rng, 80))
        live.add(3, True)
        self.assertEqual(live.read_target(rng, 0), 3)

    def test_arbitrary_removal_and_accounting(self):
        live = sim.LiveBlocks()
        requests = dict.fromkeys(range(6))
        for request_id in requests:
            live.add(request_id, request_id % 2 == 0)
        for request_id in (2, 1, 5, 0, 4, 3):
            live.remove(request_id)
            del requests[request_id]
            state = live.snapshot(requests)
            self.assertEqual(state['live_hot_blocks'] + state['live_cold_blocks'], len(requests))
        live.add(0, True)
        with self.assertRaises(RuntimeError):
            live.snapshot({})


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

    def simulation(self, progress=None, **overrides):
        config = PythonTests().config(
            sizes=[4 * KiB],
            weights=[1],
            steps=4,
            capacity=16 * KiB,
            min_water_level=50,
            max_water_level=100,
        )
        config.update(overrides)
        return sim.run_simulation(config, progress=progress)[config['mode']]

    def test_progress_after_eviction_and_failures(self):
        events = []
        original = sim.Measurement.apply

        def record(measurement, operation):
            original(measurement, operation)
            events.append(operation)

        with patch.object(sim.Measurement, 'apply', record):
            self.simulation(progress=lambda completed, total: events.append((completed, total)))
        self.assertEqual([event for event in events if isinstance(event, tuple)], [(index, 4) for index in range(5)])
        self.assertTrue(all(isinstance(event, sim.Free) for event in events[-3:-1]))
        self.assertEqual(events[-1], (4, 4))
        updates = []
        result = self.simulation(
            steps=3,
            sizes=[32 * KiB],
            progress=lambda completed, total: updates.append((completed, total)),
        )
        self.assertEqual(result['counters']['capacity_failures'], 3)
        self.assertEqual(updates, [(index, 3) for index in range(4)])

    def record_simulation(self, **overrides):
        events = []
        original = sim.Measurement.apply

        def record(measurement, operation):
            original(measurement, operation)
            events.append(operation)

        with patch.object(sim.Measurement, 'apply', record):
            result = self.simulation(**overrides)
        return result, events

    def test_lru_promotes_reads_and_zero_reads_is_fifo(self):
        for mode in ('4k-align', 'max-value-align'):
            result, events = self.record_simulation(mode=mode, eviction_policy='lru', reads_per_allocation=0)
            self.assertEqual([op.request_id for op in events if isinstance(op, sim.Free)], [0, 1])
            self.assertEqual(result['counters']['reads'], 0)
            self.assertEqual(result['counters']['skipped_reads'], 0)
            with patch.object(
                sim.LiveBlocks,
                'read_target',
                autospec=True,
                side_effect=lambda live, _rng, _percent: 0 if live.order else None,
            ):
                result, events = self.record_simulation(mode=mode, eviction_policy='lru', reads_per_allocation=1)
            self.assertEqual([op.request_id for op in events if isinstance(op, sim.Free)], [1, 2])
            self.assertEqual(result['counters']['reads'], 3)
            self.assertEqual(result['counters']['skipped_reads'], 1)
            self.assertEqual(result['final']['operation_index'], 6)
            self.assertEqual(result['final']['ranges'], [(4 * KiB, 8 * KiB)])

    def test_read_counters_and_group_endpoints(self):
        for policy in ('random', 'lru'):
            for hot in (0, 100):
                for hot_read in (0, 100):
                    result = self.simulation(eviction_policy=policy, hot_block_percent=hot, hot_read_percent=hot_read)
                    counts = result['counters']
                    self.assertEqual(counts['reads'], 12)
                    self.assertEqual(counts['skipped_reads'], 4)
                    self.assertEqual(counts['hot_reads'], 12 if hot else 0)
                    self.assertEqual(counts['cold_reads'], 0 if hot else 12)
                    self.assertEqual(counts['hot_assignments'], 4 if hot else 0)
                    self.assertEqual(counts['cold_assignments'], 0 if hot else 4)
                    self.assertEqual(result['final']['live_hot_blocks'], 2 if hot else 0)
                    self.assertEqual(result['final']['live_cold_blocks'], 0 if hot else 2)
        result = self.simulation(steps=5, min_water_level=0, eviction_policy='lru')
        self.assertEqual(result['counters']['reads'], 12)
        self.assertEqual(result['counters']['skipped_reads'], 8)

    def test_access_streams_and_random_eviction_compatibility(self):
        _, events = self.record_simulation()
        self.assertEqual([op.request_id for op in events if isinstance(op, sim.Free)], [0, 3])
        options = {'sizes': [4 * KiB, 8 * KiB], 'weights': [0.5, 0.5], 'steps': 100}
        original, events = self.record_simulation(**options, reads_per_allocation=0, hot_block_percent=0)
        changed, other_events = self.record_simulation(**options, reads_per_allocation=7, hot_block_percent=100)
        self.assertEqual(events, other_events)
        self.assertEqual(original['final']['ranges'], changed['final']['ranges'])
        lru, lru_events = self.record_simulation(**options, eviction_policy='lru')
        self.assertEqual(lru, self.simulation(**options, eviction_policy='lru'))
        self.assertEqual(
            [op for op in events if isinstance(op, sim.Alloc)],
            [op for op in lru_events if isinstance(op, sim.Alloc)],
        )

    def test_invalid_read_aborts_without_reports(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(sim, 'build_native', return_value=self.native),
            patch.object(sim.LiveBlocks, 'read_target', return_value=999),
            contextlib.redirect_stderr(io.StringIO()) as error,
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            path = Path(directory) / 'report.json'
            self.assertEqual(sim.main(['--json-report-path', str(path)]), 1)
            self.assertFalse(path.exists())
        self.assertEqual(output.getvalue(), '')
        self.assertIn('Allocation attempt 1, Operation 0, Read 1', error.getvalue())
        self.assertIn('not a live allocation', error.getvalue())

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
        self.assertEqual(result['counters']['reads'], 8)
        self.assertEqual(result['counters']['hot_assignments'] + result['counters']['cold_assignments'], 1)

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
        updates = []

        def fail_release(measurement, operation):
            if isinstance(operation, sim.Free):
                raise RuntimeError('Native release failed.')  # noqa: TRY004 - simulate a native release failure
            return original(measurement, operation)

        with (
            patch.object(sim.Measurement, 'apply', fail_release),
            self.assertRaisesRegex(RuntimeError, 'Allocation attempt 4, Operation 5: Native release failed'),
        ):
            self.simulation(progress=lambda completed, total: updates.append((completed, total)))
        self.assertEqual(updates[-1], (3, 4))
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
            self.assertEqual(rows[-1]['eviction_policy'], 'random')
            self.assertEqual(rows[-1]['reads'], '12')
            self.assertEqual(rows[-1]['skipped_reads'], '4')
            self.assertEqual(int(rows[-1]['live_hot_blocks']) + int(rows[-1]['live_cold_blocks']), 2)

    def test_lru_reports_and_python_defaults(self):
        config = PythonTests().config()
        unchanged = dict(config)
        defaults = sim.run_simulation(config)
        self.assertEqual(config, unchanged)
        self.assertEqual(defaults, sim.run_simulation({**sim.SIMULATION_DEFAULTS, **config}))
        result = self.simulation(eviction_policy='lru', hot_block_percent=100)
        configuration = {
            **PythonTests().config(),
            **sim.SIMULATION_DEFAULTS,
            'eviction_policy': 'lru',
            'hot_block_percent': 100,
        }
        report = {'configuration': configuration, 'modes': {'4k-align': result}}
        with contextlib.redirect_stdout(io.StringIO()) as output:
            sim.print_report(report)
        self.assertIn('Eviction policy: lru', output.getvalue())
        self.assertIn('Live blocks: 2 hot, 0 cold', output.getvalue())
        self.assertIn('"hot_block_percent": "100%"', output.getvalue())
        self.assertEqual(configuration['hot_block_percent'], 100)
        with tempfile.TemporaryDirectory() as directory:
            json_path, csv_path = Path(directory) / 'out.json', Path(directory) / 'out.csv'
            sim.write_reports(report, json_path, csv_path)
            saved = json.loads(json_path.read_text())
            self.assertEqual(saved['configuration']['hot_block_percent'], 100)
            self.assertEqual(saved['modes']['4k-align']['counters']['hot_reads'], 12)
            with csv_path.open(encoding='utf-8', newline='') as stream:
                row = list(csv.DictReader(stream))[-1]
            self.assertEqual(row['eviction_policy'], 'lru')
            self.assertEqual(row['live_hot_blocks'], '2')


if __name__ == '__main__':
    unittest.main()
