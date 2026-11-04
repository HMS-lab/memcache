#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Standalone workloads and measurements for the real MemCache blob allocator."""

import argparse
import csv
import hashlib
import importlib.util
import json
import os
import random
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from collections import Counter, OrderedDict
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path

TOOL = Path(__file__).resolve().parent
ALIGNMENT = 4096
MAX_SIZE = (1 << 64) - ALIGNMENT
PERCENT = 100
SIMULATION_DEFAULTS = {
    'eviction_policy': 'random',
    'reads_per_allocation': 4,
    'hot_block_percent': 20,
    'hot_read_percent': 80,
}
SIMULATION_COUNTERS = (
    'eviction_cycles',
    'evictions',
    'evicted_reserved_bytes',
    'reads',
    'hot_reads',
    'cold_reads',
    'skipped_reads',
    'hot_assignments',
    'cold_assignments',
)


def size_value(text):
    match = re.fullmatch(r'(\d+)(B|KiB|MiB|GiB|TiB)?', str(text).strip(), re.IGNORECASE)
    if not match:
        raise ValueError(f'Invalid size: {text}. Use integer bytes or binary units such as KiB.')
    unit = (match[2] or 'B').lower()
    size = int(match[1]) * 1024 ** ['b', 'kib', 'mib', 'gib', 'tib'].index(unit)
    if not 0 < size <= MAX_SIZE:
        raise ValueError('Size must be positive and must not overflow alignment.')
    return size


def distribution(text):
    entries = [item.strip().split(':') for item in text.split(',')]
    if any(len(item) not in (1, 2) for item in entries):
        raise ValueError('Use SIZE or SIZE:PERCENT% for each entry.')
    sizes = [size_value(item[0]) for item in entries]
    if len(set(sizes)) != len(sizes):
        raise ValueError('Duplicate block sizes are invalid.')
    weighted = [len(item) == 2 for item in entries]
    if any(weighted) and not all(weighted):
        raise ValueError('Supply all percentages or omit all percentages.')
    if all(weighted):
        if any(not re.fullmatch(r'\d+(?:\.\d+)?%', item[1]) for item in entries):
            raise ValueError('Each percentage must be a positive number followed by %.')
        weights = [Decimal(item[1][:-1]) for item in entries]
        if any(weight <= 0 for weight in weights) or sum(weights) != PERCENT:
            raise ValueError('Positive percentages must total 100%.')
    else:
        weights = [Decimal(1)] * len(sizes)
    total = sum(weights)
    return sizes, [float(weight / total) for weight in weights]


def align(size):
    return (size + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT


@dataclass(frozen=True)
class Alloc:
    request_id: int
    size: int
    phase: str = ''


@dataclass(frozen=True)
class Free:
    request_id: int
    phase: str = ''


def operations(config):
    rng = random.Random(config['seed'])
    for index in range(config['steps']):
        yield Alloc(
            index,
            rng.choices(config['sizes'], weights=config['weights'], k=1)[0],
            'alloc',
        )


def build_native():
    if not shutil.which('cmake'):
        raise RuntimeError('CMake is missing. Install CMake 3.18 or newer before running a measurement.')
    if importlib.util.find_spec('pybind11') is None:
        raise RuntimeError('pybind11 is missing. Install it for the current Python interpreter.')
    # A stable interpreter identity also separates virtual environments.

    identity = sys.implementation.cache_tag + '-' + hashlib.sha256(sys.executable.encode()).hexdigest()[:12]
    build = TOOL / 'build' / identity
    command = [
        'cmake',
        '-S',
        str(TOOL),
        '-B',
        str(build),
        '-DCMAKE_BUILD_TYPE=Release',
        f'-DPython3_EXECUTABLE={sys.executable}',
    ]
    headers = os.environ.get('BLOB_SIM_JSON_HEADERS')
    # Clear stale explicit paths when the environment returns to automatic discovery.
    command.append(f'-DJSON_HEADERS={headers or ""}')
    python_headers = os.environ.get('BLOB_SIM_PYTHON_HEADERS')
    if python_headers:
        command.append(f'-DPython3_INCLUDE_DIR={python_headers}')
    subprocess.run(command, check=True, stdout=sys.stderr)
    subprocess.run(
        ['cmake', '--build', str(build), '--config', 'Release'],
        check=True,
        stdout=sys.stderr,
    )
    path = build / 'native' / ('_allocator_sim_native' + sysconfig.get_config_var('EXT_SUFFIX'))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    existing = sys.modules.get('_allocator_sim_native')
    if existing is not None:
        if existing.__binary_digest__ != digest:
            raise RuntimeError('Native code changed. Start a new Python process before measurement.')
        return existing
    spec = importlib.util.spec_from_file_location('_allocator_sim_native', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.__binary_digest__ = digest
    sys.modules[spec.name] = module
    return module


def histogram(ranges):
    buckets = {}
    exact = Counter()
    for _, size in ranges:
        if size <= 0:
            raise RuntimeError('A free range has an invalid size.')
        upper = ALIGNMENT
        while upper < size:
            upper *= 2
        count, total = buckets.get(upper, (0, 0))
        buckets[upper] = (count + 1, total + size)
        exact[size] += 1
    result = []
    upper = ALIGNMENT
    while buckets and upper <= max(buckets):
        count, total = buckets.get(upper, (0, 0))
        result.append(
            {
                'lower': 0 if upper == ALIGNMENT else upper // 2,
                'upper': upper,
                'count': count,
                'bytes': total,
            }
        )
        upper *= 2
    return result, dict(sorted(exact.items()))


def percentage(numerator, denominator):
    return PERCENT * numerator / denominator if denominator else None


class Measurement:
    def __init__(self, native, capacity, enabled):
        self.allocator = native.Allocator(capacity, enabled)
        self.requests = {}
        self.seen = set()
        self.counts = Counter(
            {
                key: 0
                for key in (
                    'attempts',
                    'successes',
                    'capacity_failures',
                    'fragmentation_failures',
                    'releases',
                    'skipped_releases',
                )
            }
        )
        self.per_size = {}
        self.requested = self.alignment_waste = self.max_padding = 0
        self.samples = []
        self.reserved = 0

    def sample(self, index, phase='', detailed=False):
        state = self.allocator.snapshot(detailed)
        state['free'] = state['capacity'] - state['reserved']
        state['requested'] = self.requested
        state['alignment_waste'] = self.alignment_waste
        state['max_value_alignment_padding'] = self.max_padding
        if detailed:
            ranges = state['ranges']
            state['free'] = sum(size for _, size in ranges)
            state['largest_free'] = max((size for _, size in ranges), default=0)
            state['free_range_count'] = len(ranges)
            state['histogram'], state['exact_size_histogram'] = histogram(ranges)
            previous_end = 0
            for address, size in ranges:
                if address < previous_end or address + size > state['capacity']:
                    raise RuntimeError('Free ranges overlap or exceed capacity.')
                previous_end = address + size
        if (
            sum(info['reserved'] for record in self.requests.values() if record is not None for _, info in [record])
            != state['reserved']
        ):
            raise RuntimeError('Live blocks disagree with reserved bytes.')
        if (
            state['reserved'] + state['free'] != state['capacity']
            or self.reserved != state['reserved']
            or self.requested + state['internal_bytes'] != state['reserved']
            or self.alignment_waste + self.max_padding != state['internal_bytes']
        ):
            raise RuntimeError('Allocator accounting is inconsistent.')
        state['internal_percent'] = percentage(state['internal_bytes'], state['reserved'])
        state['external_percent'] = percentage(state['free'] - state['largest_free'], state['free'])
        state['occupancy_percent'] = percentage(state['reserved'], state['capacity'])
        state['operation_index'] = index
        state['phase'] = phase
        scalar = {key: value for key, value in state.items() if not isinstance(value, (dict, list))}
        scalar.update(
            {
                key: self.counts[key]
                for key in (
                    'attempts',
                    'successes',
                    'capacity_failures',
                    'fragmentation_failures',
                    'releases',
                    'skipped_releases',
                )
            }
        )
        self.samples.append(scalar)
        return state

    def apply(self, operation):
        request_id = operation.request_id
        if not isinstance(request_id, int) or request_id < 0:
            raise ValueError('Request IDs must be nonnegative integers.')
        if isinstance(operation, Alloc):
            if request_id in self.seen:
                raise ValueError('Duplicate allocation request ID.')
            if not isinstance(operation.size, int) or not 0 < operation.size <= MAX_SIZE:
                raise ValueError('Allocation size is invalid or alignment would overflow.')
            self.seen.add(request_id)
            size = operation.size
            counts = self.per_size.setdefault(size, Counter())
            counts['attempts'] += 1
            self.counts['attempts'] += 1
            required = self.allocator.required_size(size)
            handle = self.allocator.alloc(size)
            if handle is None:
                state = self.allocator.snapshot(False)
                free = state['capacity'] - state['reserved']
                if required > free:
                    category = 'capacity_failures'
                elif required > state['largest_free']:
                    category = 'fragmentation_failures'
                else:
                    raise RuntimeError('Allocation failure contradicts free-space metrics.')
                self.counts[category] += 1
                counts[category] += 1
                self.requests[request_id] = None
            else:
                info = self.allocator.blob_info(handle)
                if info['requested'] != size or info['reserved'] < size:
                    raise RuntimeError('Native allocation metadata is inconsistent.')
                self.requests[request_id] = (handle, info)
                self.requested += size
                self.reserved += info['reserved']
                aligned = self.allocator.align_size(size)
                self.alignment_waste += aligned - size
                self.max_padding += info['reserved'] - aligned
                self.counts['successes'] += 1
                counts['successes'] += 1
        elif isinstance(operation, Free):
            if request_id not in self.requests:
                raise ValueError('Unknown request or double free.')
            record = self.requests.pop(request_id)
            if record is None:
                self.counts['skipped_releases'] += 1
                return
            handle, info = record
            self.allocator.free(handle)
            self.requested -= info['requested']
            self.reserved -= info['reserved']
            aligned = self.allocator.align_size(info['requested'])
            self.alignment_waste -= aligned - info['requested']
            self.max_padding -= info['reserved'] - aligned
            self.counts['releases'] += 1
        else:
            raise TypeError('Unknown operation type.')


def run_workload(sequence, capacity, mode='4k-align', sample_every=1000):
    """Build the adapter, then measure an iterable of Alloc and Free operations."""
    if not isinstance(capacity, int) or not 0 < capacity <= MAX_SIZE or sample_every <= 0:
        raise ValueError('Capacity and sample interval must be positive and valid.')
    if mode not in ('4k-align', 'max-value-align'):
        raise ValueError('Unknown mode. Use 4k-align or max-value-align.')
    native = build_native()
    modes = [mode == 'max-value-align']
    measurements = {
        ('max-value-align' if enabled else '4k-align'): Measurement(native, capacity, enabled) for enabled in modes
    }
    for measurement in measurements.values():
        measurement.sample(0, 'initial', True)
    index = 0
    previous_phase = None
    try:
        for index, operation in enumerate(sequence, 1):
            phase = operation.phase
            if previous_phase is not None and phase != previous_phase:
                for measurement in measurements.values():
                    measurement.sample(index - 1, previous_phase, True)
            for measurement in measurements.values():
                measurement.apply(operation)
                if index % sample_every == 0:
                    measurement.sample(index, phase, True)
            previous_phase = phase
        return {
            mode: {
                'final': measurement.sample(index, 'final', True),
                'counters': dict(measurement.counts),
                'per_size': measurement.per_size,
                'samples': measurement.samples,
            }
            for mode, measurement in measurements.items()
        }
    except Exception as error:
        raise RuntimeError(f'Operation {index}: {error}') from error


class LiveBlocks:
    """Track live blocks, fixed hot/cold groups, and least-recently-used order."""

    def __init__(self):
        self.order = OrderedDict()
        self.ids = []
        self.positions = {}
        self.groups = {True: [], False: []}
        self.group_positions = {}

    def add(self, request_id, hot):
        if request_id in self.order:
            raise ValueError('The block is already live.')
        self.positions[request_id] = len(self.ids)
        self.ids.append(request_id)
        self.group_positions[request_id] = len(self.groups[hot])
        self.groups[hot].append(request_id)
        self.order[request_id] = hot

    def touch(self, request_id):
        if request_id not in self.order:
            raise ValueError('Cannot read a block that is not live.')
        self.order.move_to_end(request_id)
        return self.order[request_id]

    def remove(self, request_id):
        if request_id not in self.order:
            raise ValueError('Cannot remove a block that is not live.')
        hot = self.order[request_id]
        for ids, positions in ((self.ids, self.positions), (self.groups[hot], self.group_positions)):
            position = positions.pop(request_id)
            last = ids.pop()
            if last != request_id:
                ids[position] = last
                positions[last] = position
        del self.order[request_id]

    def read_target(self, rng, hot_percent):
        if not self.order:
            return None
        hot = rng.randrange(PERCENT) < hot_percent
        candidates = self.groups[hot] or self.groups[not hot]
        return candidates[rng.randrange(len(candidates))]

    def victim(self, policy, rng):
        if not self.order:
            raise RuntimeError('Eviction requires a live block.')
        if policy == 'lru':
            return next(iter(self.order))
        return self.ids[rng.randrange(len(self.ids))]

    def snapshot(self, requests):
        if (
            self.order.keys() != requests.keys()
            or self.positions != {request_id: index for index, request_id in enumerate(self.ids)}
            or self.positions.keys() != self.order.keys()
            or self.group_positions
            != {request_id: index for group in self.groups.values() for index, request_id in enumerate(group)}
            or self.group_positions.keys() != self.order.keys()
            or sum(len(group) for group in self.groups.values()) != len(self.order)
            or any(self.order[request_id] != hot for hot, group in self.groups.items() for request_id in group)
        ):
            raise RuntimeError('Live blocks and access bookkeeping are inconsistent.')
        return {'live_hot_blocks': len(self.groups[True]), 'live_cold_blocks': len(self.groups[False])}


class Progress:
    """Print progress to stderr at each new integer percentage."""

    def __init__(self, stream=None):
        self.stream = sys.stderr if stream is None else stream
        self.interactive = self.stream.isatty()
        self.percent = -1
        self.active = False

    def __call__(self, completed, total):
        percent = PERCENT * completed // total
        if percent <= self.percent:
            return
        self.percent = percent
        self.active = self.interactive and percent < PERCENT
        prefix = '\r' if self.interactive else ''
        print(
            f'{prefix}Progress: {percent:3d}% ({completed:,}/{total:,} allocation attempts)',
            file=self.stream,
            end='' if self.active else '\n',
            flush=True,
        )

    def close(self):
        if self.active:
            print(file=self.stream, flush=True)
            self.active = False


def run_simulation(config, progress=None):
    """Build and run allocation attempts with synchronous water-level eviction."""
    config = {**SIMULATION_DEFAULTS, **config}
    policy = config['eviction_policy']
    if policy not in ('random', 'lru'):
        raise ValueError('Unknown eviction policy. Use random or lru.')
    reads = config['reads_per_allocation']
    if type(reads) is not int or reads < 0:
        raise ValueError('Reads per allocation must be a nonnegative integer.')
    for key in ('hot_block_percent', 'hot_read_percent'):
        if type(config[key]) is not int or not 0 <= config[key] <= PERCENT:
            raise ValueError(f'{key} must be an integer from 0 through 100.')
    low, high = config['min_water_level'], config['max_water_level']
    if any(type(value) is not int for value in (low, high)) or not 0 <= low < high <= PERCENT:
        raise ValueError('Water levels must be integers: 0 <= minimum < maximum <= 100.')
    if config['steps'] <= 0 or config['sample_every'] <= 0:
        raise ValueError('Counts must be positive.')
    capacity = config['capacity']
    if not isinstance(capacity, int) or not 0 < capacity <= MAX_SIZE:
        raise ValueError('Capacity must be positive and valid.')
    mode = config['mode']
    if mode not in ('4k-align', 'max-value-align'):
        raise ValueError('Unknown mode. Use 4k-align or max-value-align.')
    measurement = Measurement(build_native(), capacity, mode == 'max-value-align')
    measurement.counts.update({key: 0 for key in SIMULATION_COUNTERS})
    rng = random.Random(config['seed'])
    group_rng = random.Random(f'{config["seed"]}:hot-groups')
    read_rng = random.Random(f'{config["seed"]}:reads')
    live = LiveBlocks()
    index = allocation_index = read_index = 0

    def sample(phase):
        state = measurement.sample(index, phase, True)
        state['allocation_index'] = allocation_index
        state['eviction_policy'] = policy
        state.update(live.snapshot(measurement.requests))
        measurement.samples[-1].update(
            {
                key: state[key]
                for key in (
                    'allocation_index',
                    'eviction_policy',
                    'live_hot_blocks',
                    'live_cold_blocks',
                )
            }
        )
        measurement.samples[-1].update({key: measurement.counts[key] for key in SIMULATION_COUNTERS})
        return state

    sample('initial')
    if progress is not None:
        progress(0, config['steps'])
    try:
        for operation in operations(config):
            allocation_index += 1
            for read_index in range(1, reads + 1):
                target = live.read_target(read_rng, config['hot_read_percent'])
                if target is None:
                    measurement.counts['skipped_reads'] += 1
                else:
                    if target not in measurement.requests:
                        raise RuntimeError('The read target is not a live allocation.')
                    hot = live.touch(target)
                    measurement.counts['reads'] += 1
                    measurement.counts['hot_reads' if hot else 'cold_reads'] += 1
            read_index = 0
            hot = group_rng.randrange(PERCENT) < config['hot_block_percent']
            index += 1
            measurement.apply(operation)
            if measurement.requests[operation.request_id] is None:
                del measurement.requests[operation.request_id]
            else:
                live.add(operation.request_id, hot)
                measurement.counts['hot_assignments' if hot else 'cold_assignments'] += 1
                if PERCENT * measurement.reserved >= high * capacity:
                    sample('eviction-start')
                    while PERCENT * measurement.reserved > low * capacity:
                        request_id = live.victim(policy, rng)
                        reserved = measurement.requests[request_id][1]['reserved']
                        index += 1
                        measurement.apply(Free(request_id, 'evict'))
                        live.remove(request_id)
                        measurement.counts['evictions'] += 1
                        measurement.counts['evicted_reserved_bytes'] += reserved
                    measurement.counts['eviction_cycles'] += 1
                    sample('eviction-end')
            if allocation_index % config['sample_every'] == 0:
                sample('periodic')
            if progress is not None and allocation_index < config['steps']:
                progress(allocation_index, config['steps'])
        final = sample('final')
        if progress is not None:
            progress(allocation_index, config['steps'])
        return {
            mode: {
                'final': final,
                'counters': dict(measurement.counts),
                'per_size': measurement.per_size,
                'samples': measurement.samples,
            }
        }
    except Exception as error:
        context = f'Allocation attempt {allocation_index}, Operation {index}'
        if read_index:
            context += f', Read {read_index}'
        raise RuntimeError(f'{context}: {error}') from error


def parser():
    result = argparse.ArgumentParser(
        description='Measure the real C++ blob allocator.',
    )
    result.add_argument('--capacity', default='1GiB', help='Region size. Default: 1GiB.')
    result.add_argument(
        '--block-sizes',
        '--block-size',
        default='4KiB,64KiB,1MiB',
        help='Request sizes and optional probabilities. Default: 4KiB,64KiB,1MiB.',
    )
    result.add_argument(
        '--steps',
        type=int,
        default=100000,
        help='Allocation attempts. Default: 100000.',
    )
    result.add_argument(
        '--min-water-level',
        type=int,
        default=80,
        help='Stop eviction at or below this percent. Default: 80. Use an integer without a percent sign.',
    )
    result.add_argument(
        '--max-water-level',
        type=int,
        default=90,
        help='Start eviction at or above this percent. Default: 90. Use an integer without a percent sign.',
    )
    result.add_argument(
        '--eviction-policy',
        choices=['random', 'lru'],
        default=SIMULATION_DEFAULTS['eviction_policy'],
        help='Evict a random block or the least recently used block. Default: random.',
    )
    result.add_argument(
        '--reads-per-allocation',
        type=int,
        default=SIMULATION_DEFAULTS['reads_per_allocation'],
        help='Synthetic reads before each allocation attempt. Default: 4. Use a nonnegative integer.',
    )
    result.add_argument(
        '--hot-block-percent',
        type=int,
        default=SIMULATION_DEFAULTS['hot_block_percent'],
        help='Hot assignment probability. Default: 20. Use an integer from 0 through 100, without a percent sign.',
    )
    result.add_argument(
        '--hot-read-percent',
        type=int,
        default=SIMULATION_DEFAULTS['hot_read_percent'],
        help='Hot group selection probability per read. Default: 80. Use an integer from 0 through 100.',
    )
    result.add_argument(
        '--seed',
        type=int,
        default=42,
        help='Seed for sizes, eviction targets, groups, and reads. Default: 42.',
    )
    result.add_argument(
        '--mode',
        choices=['4k-align', 'max-value-align'],
        default='4k-align',
        help='Alignment policy. Default: 4k-align.',
    )
    result.add_argument(
        '--sample-every',
        type=int,
        default=1000,
        help='Sample interval in allocation attempts. Default: 1000.',
    )
    result.add_argument(
        '--json-report-path',
        type=Path,
        help='Optional JSON destination. Default: no file.',
    )
    result.add_argument(
        '--csv-report-path',
        type=Path,
        help='Optional CSV destination. Default: no file.',
    )
    return result


def human_size(size):
    units = ('B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB', 'EiB')
    value = size
    for unit in units:
        if value < 1024 or unit == units[-1]:
            return f'{value:.2f} {unit}' if unit != 'B' else f'{value} B'
        value /= 1024
    raise AssertionError('Unreachable size unit.')


def readable_bytes(size):
    return f'{size:,} bytes ({human_size(size)})'


def print_table(headers, rows):
    rows = [[str(cell) for cell in row] for row in rows]
    widths = [max([len(header), *(len(row[index]) for row in rows)]) for index, header in enumerate(headers)]
    for row in (headers, ['-' * width for width in widths], *rows):
        print(
            '  '.join(
                f'{cell:<{width}}' if index == 0 else f'{cell:>{width}}'
                for index, (cell, width) in enumerate(zip(row, widths, strict=True))
            )
        )


def print_report(report):
    configuration = dict(report['configuration'])
    configuration['capacity'] = readable_bytes(configuration['capacity'])
    for key in ('min_water_level', 'max_water_level', 'hot_block_percent', 'hot_read_percent'):
        if key in configuration:
            configuration[key] = f'{configuration[key]}%'
    configuration['sizes'] = [readable_bytes(size) for size in configuration['sizes']]
    configuration['weights'] = [
        f'{human_size(size)} : {weight * PERCENT:.2f}%'
        for size, weight in zip(
            report['configuration']['sizes'],
            report['configuration']['weights'],
            strict=True,
        )
    ]
    print('Configuration:')
    print(json.dumps(configuration, indent=2, sort_keys=True))
    for mode, outcome in report['modes'].items():
        state = outcome['final']
        print(f'\nMode: {mode}')
        if 'eviction_policy' in state:
            print(f'Eviction policy: {state["eviction_policy"]}')
        print('\nCounters:')
        print_table(
            ['Counter', 'Value'],
            [
                (key.replace('_', ' '), readable_bytes(value) if key == 'evicted_reserved_bytes' else f'{value:,}')
                for key, value in outcome['counters'].items()
            ],
        )
        print('\nMemory:')
        byte_metrics = (
            ('capacity', 'Capacity'),
            ('requested', 'Requested'),
            ('reserved', 'Reserved'),
            ('free', 'Free'),
            ('internal_bytes', 'Internal fragmentation'),
            ('alignment_waste', 'Alignment waste'),
            ('max_value_alignment_padding', 'Max-value-align padding'),
            ('largest_free', 'Largest free range'),
        )
        print_table(
            ['Metric', 'Size'],
            [(label, readable_bytes(state[key])) for key, label in byte_metrics],
        )
        print(f'Occupancy: {state["occupancy_percent"]:.2f}% (denominator: capacity bytes)')
        print(f'Free ranges: {state["free_range_count"]:,}')
        if 'live_hot_blocks' in state:
            print(f'Live blocks: {state["live_hot_blocks"]:,} hot, {state["live_cold_blocks"]:,} cold')
        for key, denominator in [
            ('internal_percent', 'reserved bytes'),
            ('external_percent', 'free bytes'),
        ]:
            value = state[key]
            formatted = 'N/A' if value is None else f'{value:.2f}%'
            label = 'Internal fragmentation' if key == 'internal_percent' else 'External fragmentation'
            print(f'{label}: {formatted} (denominator: {denominator})')
        print('\nAllocation attempts by size:')
        categories = (
            'attempts',
            'successes',
            'capacity_failures',
            'fragmentation_failures',
        )
        print_table(
            [
                'Requested size',
                'Attempts',
                'Successes',
                'Capacity failures',
                'Fragmentation failures',
            ],
            [
                [
                    readable_bytes(int(size)),
                    *(f'{counts.get(key, 0):,}' for key in categories),
                ]
                for size, counts in sorted(outcome['per_size'].items(), key=lambda item: int(item[0]))
            ],
        )
        print('\nFree-range histogram:')
        print_table(
            [
                'Range size (lower exclusive, upper inclusive)',
                'Count',
                'Actual free space',
            ],
            [
                [
                    f'({human_size(bucket["lower"])}, {human_size(bucket["upper"])}]',
                    f'{bucket["count"]:,}',
                    readable_bytes(bucket['bytes']),
                ]
                for bucket in state['histogram']
            ],
        )


def write_reports(report, json_path, csv_path):
    """Prepare all requested files before publishing any report."""
    paths = [path.resolve() for path in (json_path, csv_path) if path is not None]
    if len(set(paths)) != len(paths):
        raise ValueError('JSON and CSV destinations must differ.')
    staged = []
    try:
        for path, kind in ((json_path, 'json'), (csv_path, 'csv')):
            if path is None:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode='w', encoding='utf-8', newline='', dir=path.parent, delete=False
            ) as stream:
                staged.append((Path(stream.name), path))
                if kind == 'json':
                    json.dump(report, stream, indent=2)
                    stream.write('\n')
                else:
                    rows = [
                        dict(mode=mode, **sample)
                        for mode, outcome in report['modes'].items()
                        for sample in outcome['samples']
                    ]
                    fields = sorted(set().union(*(row.keys() for row in rows)))
                    writer = csv.DictWriter(stream, fieldnames=fields)
                    writer.writeheader()
                    writer.writerows(rows)
        for temporary, destination in staged:
            temporary.replace(destination)
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        sizes, weights = distribution(args.block_sizes)
        capacity = size_value(args.capacity)
        if args.steps <= 0 or args.sample_every <= 0:
            raise ValueError('Counts must be positive.')
        if not 0 <= args.min_water_level < args.max_water_level <= PERCENT:
            raise ValueError('Water levels must satisfy 0 <= minimum < maximum <= 100.')
        config = {
            'capacity': capacity,
            'sizes': sizes,
            'weights': weights,
            'steps': args.steps,
            'min_water_level': args.min_water_level,
            'max_water_level': args.max_water_level,
            'seed': args.seed,
            'workload': 'water-level',
            'mode': args.mode,
            'sample_every': args.sample_every,
            **{key: getattr(args, key) for key in SIMULATION_DEFAULTS},
        }
        progress = Progress()
        try:
            modes = run_simulation(config, progress=progress)
        finally:
            progress.close()
        report = {'configuration': config, 'modes': modes}
        repo = TOOL.parents[3]
        if shutil.which('git'):
            revision = subprocess.run(
                ['git', '-C', str(repo), 'rev-parse', 'HEAD'],
                capture_output=True,
                text=True,
                check=False,
            )
            report['source_revision'] = revision.stdout.strip() if revision.returncode == 0 else None
        write_reports(report, args.json_report_path, args.csv_report_path)
        print_report(report)
        return 0
    except (
        ValueError,
        RuntimeError,
        OSError,
        subprocess.CalledProcessError,
        ImportError,
    ) as error:
        print(f'Measurement aborted: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
