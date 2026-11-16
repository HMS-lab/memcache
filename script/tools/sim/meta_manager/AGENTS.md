# MetaManager simulator guide

## Scope

- Keep changes in this simulator directory. Shared MemCache C++ and sibling
  simulators are read-only.
- The finalized `src/meta_manager_sim/workloads/`, `.githooks/`, `format.sh`
  and `setup.cfg` are read-only. Reading them is allowed; formatting must leave
  their contents unchanged.
- Preserve the user's working-tree and staged changes. Do not commit, push,
  tag, publish or upload local code, data or logs. Local commands and dependency
  downloads are allowed.
- Put generated files, captures and experiments in `build/`. Keep examples
  self-contained with tracked inputs; the root `traces.jsonl` is ignored.

## Structure

```text
sim_random.py                   Seeded random reads
sim_sequential.py               Consecutive block reads
sim_trace.py                    Reads from one JSONL conversation
src/meta_manager_sim/
  cli/                          Shared CLI arguments and size parsers
  config.py                     Manager/run settings and RunConfig.from_args
  workloads/random/             Seeded generator and random workload adapter
  workloads/sequential/         Consecutive keys with one fixed block size
  workloads/trace/              JSONL reader and trace workload adapter
  simulation.py                 Sequential replay, RSS samples and tables
  metrics/                      RSS reading, summaries and plot export
  native/
    build.py                    Incremental standalone CMake build
    native_process.py           Process lifecycle, JSONL IPC and Heaptrack
    src/                        Command loop, manager adapter and local shim
examples/                       Bash examples and bundled trace input
tests/                          Pytest unit and native integration tests
pyproject.toml                  Runtime dependencies and tool settings
```

Use [README.md](README.md) for the CLI, defaults and examples. CLI defaults come
from `cli/arguments.py`; programmatic defaults in `config.py` can differ.

## Simulation contract

- Workloads supply typed `(key, size_bytes)` reads. Python sends them
  sequentially to one persistent C++ worker, one read per request. Do not add
  Python threads, a virtual clock or replay wall-time/throughput metrics.
- Trace timestamps determine ordering only. Keys are `model:hash`, without a
  conversation-line component. Seed random workload generation.
- Sequential workloads read `sequential:0`, `sequential:1`, etc. once each,
  using the same configured byte size for every block.
- Convert workload keys to the native `MODEL_BLOCK_tp_0_pp_0_HASH` format
  before sending them. Normalize model-name hyphens to underscores, then
  compute SHA-256 of `MODEL_BLOCK_tp_0_pp_0`. Send the original model
  separately for statistics.
- MetaManager owns placement, alignment, residency and eviction. Resolve reads
  through native `Get`; a hit completes its read lease, and a miss attempts one
  `Alloc`. Use one DRAM backing blob and native random placement without
  preferred ranks. Record allocation failures and continue with the next read.
  Do not maintain a separate Python cache or estimate native allocation sizes.
- Complete read/write leases with the original operation ID and the location
  returned by C++. After each successful allocation/write, call the eviction
  check exactly once and wait for native completion, including removal callbacks.
  Pass watermarks unchanged; never force occupancy below the low watermark.
  Preserve the completion check: eviction must be idle and `alloc_ok` must equal
  cached keys plus completed removal callbacks in this one-blob model.
- Keep the Python/C++ JSONL fields synchronized. `init` takes `config` and
  `detailed_stats`, returning flat `pid`, `progress`, `pools` and `details`
  fields. `read` takes `reads=[(key, size)]` and `model`, returning counters.
  `snapshot` returns final state, and `stop` shuts down the manager. There is
  no version field or simulator batch-size setting.
- Measure RSS from the actual C++ PID, including under Heaptrack. RSS covers
  the whole worker process, including runtime and adapter overhead. Keep only
  read counts, RSS and cached-key counts in memory samples; timing breakdowns
  come from native operations. A sampled RSS maximum is not a transient peak.
- Heaptrack is optional. Keep its capture and matching executable together;
  let Heaptrack handle instrumentation and analysis. Do not add custom malloc
  hooks, class-size estimates, capture parsers or profiler backend frameworks.
- Build DataFrames after worker shutdown. Preserve integer counters and byte
  counts; format percentages and MiB on display copies. Undefined rates stay
  `NaN` and print as `n/a`. Detailed tables are absent when collection is off.
- Plots are the simulator's only file export. JSONL is internal IPC; Heaptrack
  produces its own capture. Do not reintroduce JSON/CSV result export.

## Coding style

- Prefer short, direct functions and cohesive modules. Extract private helpers
  for meaningful blocks, keep classes self-contained, and avoid one-use layers
  that obscure the actual logic.
- Use explicit parameter and return annotations, including constructors, for
  new or edited functions. Prefer precise built-in containers and `DataFrame`
  for metric tables; confine broad JSON types to the process/file boundary.
- Import types directly for annotations: `Tensor`, `ArgumentParser` and
  `DataFrame`, rather than `torch.Tensor`, `argparse.ArgumentParser` or
  `pd.DataFrame`. This is a style rule, not a request to add dependencies.
- Preserve copyright/Mulan PSL v2 headers. New source files need the same
  header as their neighbours, followed by one blank line before imports or
  code. Keep a Bash shebang first. Do not add module-level docstrings.
- Add comments conservatively: short English sentences without trailing
  periods. Use `# Note:` in Python or `// Note:` in C++ for non-obvious reasons,
  constraints or implementation choices. Avoid comments that repeat the code.
- Python uses Black/isort at 79 columns, with magic trailing commas disabled;
  pycodestyle ignores `E203,W503`. Keep the existing section separators and
  related logic together.
- Native code uses C++17, clang-format 18.1.8 and the repository's 120-column
  [.clang-format](../../../../.clang-format). Follow the
  [C++ naming guide](../../../../docs/zh/c_cpp_naming.md): UpperCamelCase types
  and functions, lowerCamelCase locals/parameters, private members ending in
  `_`, and named constants for values with a specific meaning.
- Trust established internal interfaces and valid workload data. Validate
  real external boundaries where needed; avoid repeated checks, speculative
  fallbacks, retries, compatibility layers and extra configuration.
- Runtime dependencies belong in `pyproject.toml`. Test/formatting tools are
  installed separately; add dependencies only for an actual use.

## Running and building

Follow the README setup and activate `.venv`. All three entrypoints support
`--help` without building or launching the native worker. Examples:

```bash
bash examples/random_example.sh
bash examples/sequential_example.sh --num-reads 8 --block-size 4KiB
bash examples/trace_example.sh
```

The trace example enables `--profile` and requires Heaptrack on PATH. Examples
save their plots in `build/`; profiling captures go in `build/profiles/`.
The sequential example defaults to a million reads; the override above keeps
the run short.

Native code rebuilds automatically before a simulation. To build it alone:

```bash
python -c 'from meta_manager_sim.native.build import build_native; print(build_native())'
```

Use this local standalone target, not the repository's full service build.
The simulator requires the surrounding MemCache checkout, Linux procfs, a
C++17 compiler and Make/Ninja; it needs no running MetaService or NPU stack.

## Formatting

Install development tools only when needed:

```bash
python -m pip install black isort pycodestyle clang-format==18.1.8
```

`bash format.sh` formats the entrypoints, `src/` and `tests/`, including the
protected workloads. For agent changes, run its individual tools only on
edited, unprotected files:

```bash
python -m isort path/to/file.py
python -m black path/to/file.py
python -m pycodestyle path/to/file.py
clang-format -i path/to/file.cpp
```

`bash .githooks/pre-push` performs the full formatting checks without pushing.
Do not run `.githooks/pre-commit` as a check: it formats files and re-adds
originally staged files, changing the index.

## Testing

Install `pytest` separately. Activate `.venv` so the native build and examples
use the same tools, then run the whole suite from this directory:

```bash
python -m pip install pytest
MPLBACKEND=Agg python -m pytest -q
```

Tests marked `native` build/run the real standalone worker, including the Bash
examples. The Heaptrack test and profiled trace example skip when Heaptrack is
missing; unprofiled trace CLI coverage still runs.
To run only the Python checks without building C++:

```bash
MPLBACKEND=Agg python -m pytest -q -m "not native"
```

Keep coverage focused:

- `test_workloads.py`: seeded reads, fixed per-key sizes, hot/cold selection,
  consecutive sequential keys, empty sequential workloads, trace ordering,
  nested requests, model keys and line selection
- `test_config.py`: size/group parsing and CLI-to-config conversion
- `test_metrics.py`: RSS units, exact integer totals, signed RSS changes,
  display formatting and the actual coordinates exported in plots
- `test_simulation.py`: real hits/misses, allocation failures, prefix/size
  breakdowns, native timings, sampling, unequal pools, both eviction policies,
  empty workloads and cleanup after an input failure
- `test_examples.py`: all three Bash examples, trace line selection and plot
  export; all three entrypoints support help without build tools
- `test_profile.py`: optional real Heaptrack capture and native-PID sampling

Prefer deterministic workloads and accounting invariants to exact timing/RSS
values, arbitrary output wording or particular eviction victims. Do not mock
MetaManager, repeat the same native scenario across modules, test argparse or
pandas themselves, or retain tests for removed APIs. Reuse a native result
when several assertions concern the same run. Keep large traces and profiler
benchmarks out of the suite; inspect plots manually when changing rendering.
Override the sequential example's million-read default in tests.
Report exactly which checks ran and any optional skips.
