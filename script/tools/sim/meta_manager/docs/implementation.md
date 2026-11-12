# Implementation and checks

The CLI drivers prepare access streams and call `run_simulation()`. The
coordinator owns scheduling, counters, and lease tokens; `ManagerSession`
translates accesses into real native calls. Native MetaManager owns keys,
readability, allocation, recency, and eviction. The
[design](design.md) describes these boundaries and their accuracy limits.

Native build sources and type stubs live under `cpp_build/`, workload generators
under `workloads/`, and observations, aggregation, and output under `profiling/`.
The standalone extension links repository sources directly. Build products,
trace spools, and generated reports are local outputs, not input dependencies.

## Run checks

After the [README setup](../README.md#setup-and-examples), run from the tool
root with the virtual environment active:

```bash
python examples/random_example.py
python examples/trace_example.py
python -m pytest -q
python -m mypy --strict src/meta_manager_sim sim_random.py sim_trace.py examples
```

The suite checks native miss/allocation/write completion, hits/read completion,
pending writes and duplicate allocation, leases, removal/reallocation, both
LRU containers, watermarks, background cleanup, allocation failures by size,
padding, alignment, native fragmentation recovery, shared trace keys, virtual
overlap, invalid inputs, metrics, report exports, examples, and help without
building.

The bundled trace has 17 accesses to seven unique keys. With the example's
4/8 KiB sizes and 64 KiB pool, expect 10 hits, seven successful allocations,
no allocation failures, and 40,960 final reserved bytes. Random input is seeded;
eviction scheduling can change cache outcomes when capacity is under pressure.

Tests exercise the current native implementation; they do not establish native
race freedom or physical payload performance.

## Formatting

`format.sh` applies isort, Black, and pycodestyle to the CLI drivers, `src/`,
and `tests/`. To format those files and the examples:

```bash
PYTHON=python bash format.sh
python -m isort examples
python -m black examples/random_example.py
python -m black examples/trace_example.py
python -m pycodestyle examples
```

For checks without rewriting files, add `--check-only` to isort and `--check`
to Black. Use Black on one file per invocation if its process workers hang in
a restricted environment. Local style settings use 79 columns and are defined
in `pyproject.toml` and `setup.cfg`.
