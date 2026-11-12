# MetaManager simulator design

This document describes the current implementation. See [README](../README.md)
for setup and commands, [metrics](metrics.md) for report definitions, and
[implementation](implementation.md) for checks.

## Scope and structure

The simulator links the real MemCache manager, containers, leases, global
allocator, and blob allocator from the repository. Python supplies accesses
and simulated completions. One DRAM pool is mounted at rank 0 with one backing
blob per key. Addresses describe the simulated pool; payload memory is not
allocated or transferred. Lower storage tiers, replicas, and service RPCs are
outside this setup.

```text
sim_random.py / sim_trace.py     CLI parsing and workload preparation
examples/                       Small Python examples and bundled trace
src/meta_manager_sim/
  config.py                     ManagerConfig and RunConfig validation
  simulation.py                 Shared coordinator and public runner
  session.py                    Native access flow and lease completion
  cpp_build/                    CMake build, bindings, adapter, and type stubs
  workloads/
    types.py                    Access, Workload, and WorkloadInfo
    random/random_workload.py   Seeded shared-key workload
    trace/trace_reader.py       JSONL validation and sorted disk spool
    trace/trace_workload.py     Trace normalization into Access streams
  profiling/
    models.py                   Detached observations and result types
    metrics.py                  Counters and derived summaries
    reporting.py                Terminal, JSON, and CSV output
  utils/                        Shared CLI handling and byte-size parsing
```

The shared repository sources remain outside this tool. The standalone build
uses local CMake, the running interpreter's pybind11, and nlohmann JSON headers.
It needs no service, NPU, CANN, or wheel build. Native artifacts and trace spools
are generated under `build/`. `@build_required` prepares the extension before
`run_simulation()`; imports and argument help have no build side effects. A
process may have only one active manager session.

## Python API

| Component | Current interface and ownership |
| --- | --- |
| `ManagerConfig` | Capacity, watermarks, native LRU policy/shards, lease TTL, cleanup timeout, alignment |
| `RunConfig` | Ordered/concurrent mode, worker count, read/write holds in nanoseconds, sample interval |
| `Access` | Immutable key, requested bytes, arrival nanoseconds, source/request/block positions, optional model |
| `Workload` | Single-use source iterators and `WorkloadInfo`; no residency map |
| `prepare_random(RandomConfig)` | Returns a workload over a shared fixed-size key catalog |
| `prepare_trace(TraceConfig)` | Context manager that owns the sorted spool and yields a workload |
| `run_simulation(workload, manager, run=RunConfig())` | Builds as needed, runs the coordinator, and returns a detached `SimulationResult` |
| `ManagerSession` | Owns the native manager; `begin_read`, `begin_write`, `begin_access`, `complete`, `snapshot`, and `close` |
| `Metrics.record(operation, result)` | Coordinator-owned outcome, size, model, status, and timing accounting |
| `print_report` / `write_reports` | Render the detached result after native execution |

`ManagerSession` also exposes `remove`, `keys`, `check_and_evict`, and
`wait_for_cleanup` for focused experiments. The workload drivers perform reads
and allocate on misses; they do not generate explicit add/remove requests.
The native adapter releases the GIL around workload calls. Python does not
implement a second cache, allocator, or eviction policy.

Defaults are a 64 MiB pool, native LRU, 80% low/90% high watermarks, a 1,000 ms
lease TTL, a 5,000 ms cleanup timeout, ordered execution, zero holds, and only
initial/final samples. Concurrent mode defaults to four workers. Sharded LRU
requires a power-of-two shard count from 1 to 1,024; its default is 16.

## One access

1. Call real manager `Get(key)`. A hit requires success and a validated readable
   descriptor with the expected size and exactly one backing blob. It acquires
   a read lease and updates native recency.
2. A missing key, or success without a usable blob, is a miss. Before attempting
   `Alloc`, call `CheckAndEvict(DRAM, requested_size)`. Do not drain eviction
   between this check and allocation.
3. Allocation success produces a write lease. Complete it with `MMC_WRITE_OK`
   using the same operation ID and location before treating the block as
   readable. A hit is completed with `MMC_READ_FINISH`.
4. A duplicate allocation means the key already exists, for example because
   another source is writing it. Count it separately; do not complete another
   writer's lease or retry the workload access.
5. Ordinary final allocation failure is counted with its requested size and
   replay continues. Unexpected statuses, inconsistent descriptors, uncertain
   lease validity, and cleanup timeouts abort the run.

Native `Alloc` may force eviction and retry internally. Counters describe the
final manager result and explicit API attempts, not hidden allocator attempts.
A miss followed by a successful fill remains a cache miss.

## Workloads

Random workloads assign each catalog key a size once, using weighted sampling.
All sources share that catalog. Each source has a seeded read generator; size
sampling has a separate seed stream. The first configured fraction of keys is
the hot group, selected with the configured hot-read probability. The access
count is exact; stopping does not depend on an eviction occurring.

Trace input is JSONL with one conversation per line. The reader accepts `s`
and `n` requests and recursively flattens `subagent` request lists. It requires
`hash_id_scope: "local"`, a positive token `block_size`, supported model-size
mappings, nonnegative timestamps, and nonnegative integer block IDs. The
simulator deliberately uses `model:hash` keys across selected conversations
and records this scope override.

`block_size` is a token count; byte sizes come from `TraceConfig.model_sizes`
or repeated `--model-block-size` arguments. Every block uses its model's full
configured byte size. Prefix dependencies are ignored: each ID is an
independent read, and eviction of one block does not invalidate another.

Each selected line becomes one source on a shared timeline. Nested timestamps
are used as supplied, without adding the parent's time. Decimal timestamps
are rounded to integer nanoseconds using half-even rounding. Requests are
sorted by timestamp and request ordinal within a line; block-list order is
preserved. Ordered ties prefer the earlier selected line. Preparation keeps
one conversation in memory at a time, plus per-line bookkeeping, and writes a
temporary sorted spool read by the coordinator. A fingerprint covers the
selected lines and their original line numbers.

## Concurrency and virtual time

The coordinator owns source cursors, counters, leases, and two heaps: ready
accesses and pending completions. Each source has at most one active access.
The next access becomes available after the current one finishes; if its
recorded timestamp has passed, it starts at the current virtual time and
accumulates queue delay.

Ordered mode executes one access and immediately completes its lease before
advancing that source. Concurrent mode submits the currently ready source
heads at the same virtual timestamp to a bounded `ThreadPoolExecutor`. There
is no dedicated thread per trace line. The coordinator joins the submitted
calls, schedules completions at `start + read_hold_ns/write_hold_ns`, and jumps
to the next event time. Completions take precedence over starts at equal time.
Zero holds still allow simultaneous native calls within a cohort.

Holds model operation overlap without sleeping. Native call duration does not
advance virtual time, and native leases still expire against the real clock.
Lease tokens retain operation ID, location, size, model, and a conservative
real-time deadline. Missing that deadline invalidates the run. Increasing the
lease TTL can be necessary for slow hosts or diagnostic instrumentation.

The scheduler orders cohorts, not the native linearization of calls within a
cohort. Concurrent outcomes and asynchronous eviction can vary between runs,
even when the input is seeded identically.

## Eviction, cleanup, and sampling

Native LRU or sharded LRU controls eviction and lease protection. Pre-allocation
watermark checks are asynchronous. The simulator does not run the service's
periodic maintenance schedule or add a Python failure-triggered eviction loop.

Initial, optional periodic, and final memory samples are taken after submitted
workload calls have joined. Sampling waits for the adapter's supported cleanup
condition: eviction is not running and successful native allocations equal
registered keys plus removed-blob callbacks. This condition is specific to
one pool and one blob per key; it is not a general manager idle API. Waiting
does not request a new eviction. Periodic sampling can affect native scheduling
and occurs at cohort boundaries, at or after the requested access threshold.

Final samples precede destruction of remaining keys. Replay wall time includes
source iteration, accounting, periodic/final samples and their cleanup waits.
It excludes trace preparation, building, manager startup, the initial sample,
report writing, and final manager destruction. Native timers cover only the
synchronous manager calls; they exclude binding conversion and background CPU.

## Accuracy limits

The trace supplies logical accesses, not allocator events, payload durations,
frees, or measured KV-cache sizes. The bundled model sizes are synthetic. The
shared `model:hash` identity overrides the input's local-scope declaration;
matching IDs across conversations are not independently verified content hashes.

Pool padding is available as internal fragmentation. Largest free ranges,
exact external fragmentation, and a reliable capacity-versus-fragmentation
failure breakdown are unavailable at this manager boundary. Final statuses
can hide native recovery. Requested-byte totals do not measure bandwidth.

Reports mark native mutation safety as `unresolved`: the shared blob allocator
reads some ordinary usage counters outside its lock, including validation in
`Release`. This tool does not fix those shared sources or claim native race
freedom from passing tests. The current report and supported topology must
remain explicit when interpreting results.
