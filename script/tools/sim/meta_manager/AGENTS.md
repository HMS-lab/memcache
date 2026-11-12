# MetaManager simulator guide

## Scope

Keep simulator changes in this directory. Shared MemCache C++ sources and the
sibling blob allocator simulator are read-only unless explicitly authorized.
Do not copy or reimplement native manager/allocator algorithms in Python.
Do not commit or push unless requested.

The current architecture is documented in [docs/design.md](docs/design.md),
setup in [README.md](README.md), and measurements in
[docs/metrics.md](docs/metrics.md). Keep these documents aligned with the code.
Examples must use bundled inputs rather than private or ignored datasets.

## Boundaries and behavior

- The CLI entrypoints prepare workloads; `simulation.py` owns the shared loop.
  Workloads yield typed `Access` streams. Native MetaManager owns residency.
- The adapter mounts one DRAM pool and one backing blob per key. It does not
  transfer payloads or use NPU hardware, CANN, or a running MetaService.
- Trace keys are `model:hash`, shared across conversations. Report the override
  of the accepted input's local hash scope. Supply model byte sizes separately;
  token block size is not bytes. Replay blocks independently of prefixes.
- A hit must return a validated readable descriptor. Complete every read and
  successful write with the matching native operation ID and location.
  Pending/unreadable lookups are misses; duplicates are a separate outcome.
- Call the native watermark check before allocation, without waiting between
  the two calls. Eviction remains asynchronous, and native allocation may
  perform its own forced eviction and retries. Do not add Python eviction.
- Concurrent execution uses a bounded thread pool and virtual event cohorts,
  with one active access per source. Native lease expiry remains real time.
- Metrics and reporting belong under `profiling/`. Distinguish current pool
  usage from cumulative allocation bytes and measured outcomes from unavailable
  failure causes. Do not add observation calls that alter recency or leases.
- Preserve the explicit native mutation-safety limitation; passing simulator
  tests is not proof of native race freedom.

## Build and checks

The public runner uses `@build_required` to invoke an incremental local CMake
build with the running Python interpreter. Build outputs and temporary spools
belong in `build/`; imports and help must not compile or start a measurement.
Reuse an existing virtual environment when practical.

See [docs/implementation.md](docs/implementation.md) for the test, type, and
style commands. The Python suite includes real standalone native execution;
it does not require the service's hardware-dependent Python tests.

## Code style

Use cohesive files, short methods, descriptive names, and explicit type hints
on all parameters and return values, including helpers, generators, tests, and
fixtures. `self`, `cls`, and constructor return annotations may be omitted.
Use `TYPE_CHECKING` for native/circular annotations without import side effects.

Avoid speculative interfaces, redundant forwarding, and duplicate authorities.
Use `tabulate` for terminal tables and standard JSON/CSV libraries for exports.
Use stable seeded streams rather than Python `hash()` for reproducible inputs.
Do not introduce a `PERCENT = 100` constant.

Python formatting uses Black at 79 columns with magic trailing commas disabled,
isort's Black profile at 79 columns, and pycodestyle ignoring `E203,W503`.
Use the repository's C++ naming and formatting rules for native adapter changes.
New source files need copyright and Mulan PSL v2 headers matching existing files.
