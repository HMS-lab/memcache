# MetaManager simulator

A Python simulator around the real C++ `MmcMetaManager`, metadata containers,
and allocators. It replays key reads, allocates on cache misses, completes read
and write leases, and records native outcomes. It uses one DRAM pool and one
backing allocation per key, without storing payloads or requiring NPU hardware,
CANN, or a running MetaService.

## Setup and examples

Run from this directory inside the MemCache checkout. Use Python 3.10 or newer,
a C++17 compiler, Python development headers, and Make or Ninja. Python package
dependencies, including CMake 3.18 or newer, are listed in `pyproject.toml`.

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e '.[dev]'

python examples/random_example.py
python examples/trace_example.py
```

The examples use the interpreter you invoke and ordinary installed-package
imports. Edit their configuration directly, or use the CLI entrypoints below.
All example inputs are included in this checkout.

- [Random example](examples/random_example.py): 200 reads from 64 shared keys,
  fixed 4/8 KiB sizes per key, a 128 KiB pool, and periodic memory samples.
- [Trace example](examples/trace_example.py): the two conversations in
  [traces.jsonl](examples/traces.jsonl), synthetic 4/8 KiB model block sizes,
  two workers, and a modeled 1 ms write hold. Its 64 KiB pool fits all seven
  unique blocks: 17 accesses, 10 hits, and seven successful allocations.

The native adapter builds incrementally on execution into `build/`, using the
running Python interpreter. Imports and `--help` do not build anything.

## CLI

```bash
python sim_random.py --help
python sim_trace.py --help

python sim_random.py --accesses 200 --key-count 64 --capacity 128KiB \
  --size-weight 4KiB:70 --size-weight 8KiB:30 --seed 42

python sim_trace.py --trace-path examples/traces.jsonl \
  --model-block-size example-model-a:4KiB \
  --model-block-size example-model-b:8KiB --capacity 64KiB \
  --execution concurrent --workers 2 --write-hold 1ms
```

Trace `--lines` defaults to `all`; selections such as `1,3:5` use one-based,
inclusive ranges. Each trace line is a logical source. Keys are `model:hash`,
shared across conversations; the input's declared local hash scope is explicitly
overridden and reported. Blocks are independent reads. Model block sizes are
supplied in bytes, including for partial token blocks.

Ordered mode completes each access immediately. Concurrent mode uses a bounded
thread pool and a coordinator that advances virtual time between arrival and
completion events. A source starts its next block after completing its current
access. Virtual holds do not sleep or change the native lease clock.

Eviction runs asynchronously, including in ordered mode. Each allocation
attempt is preceded by the native watermark check, without waiting for eviction.
Input seeds reproduce random workloads; scheduling and eviction can change
outcomes under memory pressure.

## Results

Terminal tables are always printed. Add `--json PATH` and/or
`--csv-dir DIRECTORY` to either CLI to export schema 3 reports:

```bash
python sim_random.py --accesses 200 --key-count 64 \
  --json reports/random.json --csv-dir reports/random
```

Reports include cache hits/misses, allocation successes/failures/duplicates by
size and model, requested-byte totals, native call timings, pool occupancy and
padding, and concurrency counters. See [metrics](docs/metrics.md) for definitions.

Memory samples wait for submitted native cleanup and precede final destruction.
No payload bandwidth or exact external-fragmentation cause is measured. Native
mutation safety remains unresolved because some shared allocator counters are
read outside their lock; reports label this limitation. See the
[design and accuracy limits](docs/design.md).

## Development

```bash
python -m pytest -q
python -m mypy --strict src/meta_manager_sim sim_random.py sim_trace.py examples
```

[Implementation and checks](docs/implementation.md) describes module boundaries
and style commands. [Profiling](docs/performance.md) shows how to inspect the
bundled workload without an external dataset.
