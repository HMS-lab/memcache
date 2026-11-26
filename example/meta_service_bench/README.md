# MetaService metadata benchmark

This tool runs MetaService and simulated local-service clients in separate child processes. It measures metadata RPC performance without device memory transfers or NPU hardware. Mock BM registration supplies fake HBM capacity; backup requests are acknowledged without copying cached data.

Processes appear as `bench_coord` (coordinator), `meta_service` (server), and `bench_client_0`,
`bench_client_1`, etc. (clients, numbered from zero) in `top` and `ps`, including command-line display
(`top -c`). Linux short process names are limited to 15 visible characters; the command-line title
retains the full name when the original argument storage has enough space.

Each client process retains its `MetaNetClient` connection for registration, warmup, and server callbacks. A separate MemFabric connection submits measured requests asynchronously. Its handshake uses an invalid rank so it does not replace the registered connection. Request messages carry the registered rank where required.

All processes finish registration, validated warmup, and async connection setup before the coordinator releases the startup barrier. Once a process drains its workload, it stops its timer and waits at the teardown barrier with its clients connected. Clients disconnect only after every process has drained its requests, including dependent Updates. Both barriers are outside workload measurement. Setup errors, failed warmup, or a client exiting before a barrier completes abort the run and release waiting peers.

Ctrl+C and SIGTERM request cleanup of clients, MetaService, and perf. The coordinator waits for its
children, retries interrupted waits, and kills and reaps children that exceed the shutdown timeout.
Native children also receive a parent-death signal if the coordinator exits unexpectedly. MetaService
blocks shutdown signals before creating service threads so its main thread can perform graceful shutdown.

The Python runner uses private process groups for the benchmark and off-CPU collector. It acts as a
Linux child subreaper, adopts orphaned descendants, and stops and reaps those groups on completion,
errors, Ctrl+C, and SIGTERM. This prevents exited descendants from remaining as `<defunct>` processes
when a coordinator or collector wrapper exits early. Killing the runner itself with SIGKILL bypasses
cleanup; its remaining children must then be reaped by the system's parent process. Existing zombies
from older runs can only be reaped by their current parent; rerunning the updated benchmark cannot remove them.

## Build and verify

From the repository root, use the supported build entrypoint:

```bash
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental --build_cpp_example ON
./build/example/meta_service_bench/bench_meta_service_async_test
./build/example/meta_service_bench/bench_meta_service_barrier_test
./build/example/meta_service_bench/bench_meta_service_latency_test
```

An activated Python 3.11 virtual environment is selected automatically:

```bash
python3.11 -m venv .venv311
source .venv311/bin/activate
python -m pip install pybind11
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental --build_cpp_example ON
```

Alternatively, pass `--python /absolute/path/to/python3.11`. The selected interpreter supplies its own headers and shared-library paths, including base-installation paths for a venv. Matching Python development files are required. Incremental builds refresh the Python selection; manual CMake cache edits are unnecessary.

The local test executable is built with the example and is not installed in the runtime package. Building requires the repository dependencies, including pybind11, spdlog, and MemFabric. Standalone CMake integration is retained for existing users with installed libraries exporting internal symbols; the in-tree static-library build is recommended.

Runner profiling-handshake tests use Python's standard library and fake collector/benchmark processes:

```bash
python3 -B example/meta_service_bench/runner_test.py
```

## Native profiling symbols (Alloy/Pyroscope)

Built-in `--perf` recording starts after every client completes registration, warmup, and async transport
setup, before the startup barrier releases the workload. This applies to all `--perf-target` selections
(`meta`, `clients`, and `client0`). Recording stops after every client drains its workload, before the
teardown barrier releases clients to disconnect. Warmup and client teardown are excluded from `perf.data`
and the flamegraph uploaded from it by `runner.py`. A small amount of barrier and profiler startup overhead
can still be sampled. External Alloy collection keeps its own collection window.

When `offcpu.enabled` is true, `runner.py` starts its collector only after all clients finish warmup.
A private control socket holds the clients at the startup barrier until the runner acknowledges startup.
The runner stops and reaps the collector at the drained barrier before acknowledging client teardown.
The runner and benchmark executable must both include this control protocol. Collector startup or runtime
failure aborts the run rather than silently releasing an unprofiled workload.

Folded-output BCC tools do not expose a readiness signal. `offcpu.startup_wait_seconds` (default: 2)
provides a startup grace period while clients remain paused; increase it if BPF compilation and probe
attachment take longer on your host. A live collector after this delay does not prove attachment completed.
Collector initialization and small amounts of barrier overhead can be sampled, but warmup is excluded.
Pyroscope uploads use the workload window; MongoDB also stores `workload_started_at` and `workload_duration_s`
separately from the full run's existing timestamps. The off-CPU duration limit (`max_seconds`) includes collector
startup; set it longer than the expected workload plus startup time.
The collector uses `-p` to select the same process IDs as `perf.target`, including all their named worker
threads. `offcpu.comm_filter`, if set, further filters the folded output by thread name.

For GCC/Clang builds, the benchmark and its in-tree MemCache objects explicitly use `-g3`,
`-fno-omit-frame-pointer`, `-fno-optimize-sibling-calls`, and `-fasynchronous-unwind-tables`.
The executable retains its symbols even in a RELEASE build, exports dynamic symbols, and has a GNU build ID.
Standalone builds default to DEBUG. The DEBUG command above also disables optimization in in-tree MemCache code;
optimization can inline functions even when full debug info is present. Compare measurements using the same build flags.

These settings do not rebuild externally loaded libraries. In standalone mode this includes MemCache itself;
in both modes it includes MemFabric, libc, libstdc++, and libpython. Each needs its own symbols for complete stacks.
If using the matching MemFabric source checkout, build its DEBUG package, install it, and source that installation's
environment before launching the benchmark. For this metadata-only benchmark a CPU build is sufficient:

```bash
# Run from the matching MemFabric source root, after fetching its build dependencies.
bash script/build_and_pack_run.sh --build_mode DEBUG --xpu_type NONE --build_python OFF
```

To inspect the exact benchmark executable being used (from the MemCache repository root):

```bash
readelf -W -S build/example/meta_service_bench/bench_meta_service \
  | rg '\.(debug_info|debug_line|symtab|dynsym|eh_frame|note.gnu.build-id)\s'
nm -C build/example/meta_service_bench/bench_meta_service | rg 'Worker\(|RunClient\('
```

`AccWrk` and client names in a thread-grouped profile are labels, not function names. If deep stacks resolve but
their common outermost frames are unknown, missing libc/libstdc++ thread-start symbols are one possibility.
Use an unknown frame's address and mapped module to identify the owner; the flame graph alone cannot establish
whether it belongs to the benchmark, MemFabric, a system library, or the kernel.

Alloy must be able to read the running client's executable and mapped libraries in its mount/PID namespace,
as well as matching system-library debug files (for example, Ubuntu's `libc6-dbg` and matching libstdc++ debug symbols).
Keep the profiled binaries available and do not replace them during collection. For short runs, ensure Alloy has
time to discover the client processes and read their symbols before they exit. An executable with symbols does not
repair collector access problems or missing symbols in another library. Frame pointers help older collectors;
newer implementations may unwind using `.eh_frame` instead. See the documentation matching your Alloy version:
[unknown-symbol troubleshooting](https://grafana.com/docs/pyroscope/latest/configure-client/grafana-alloy/ebpf/troubleshooting/)
and [pyroscope.ebpf configuration](https://grafana.com/docs/alloy/latest/reference/components/pyroscope/pyroscope.ebpf/).

## Run

```bash
./build/example/meta_service_bench/bench_meta_service -p 2 -t 4 -q 16 -b 32,32 --completion-mode poll
```

| Option | Meaning | Default |
| --- | --- | --- |
| `-p` | Client process count | 4 |
| `-t` | Generator threads per process | 4 |
| `-q`, `--queue-depth` | Outstanding logical operations per generator | 1 |
| `--completion-mode` | `poll` or `wait` when no completion is available | poll |
| `-n` | Logical operations issued per thread | 5000 |
| `-r` | Put percentage, restricted to 1–99 | 50 |
| `-b` | Batch range `min,max`; a single number means `1,max` | 1,32 |
| `-w` | Warmup key count | 5000 |
| `-k` | Legacy key-space setting; reads select warmup keys | 20000 |
| `-s` | Blob size in bytes | 32768 |
| `-c` | Mock HBM capacity per process in MB | 16384 |
| `-l` | Library log file | bench_meta_service.log |
| `--perf` | Profile the MetaService process into perf.data | off |
| `--perf-freq` | Perf sampling frequency | 99 |
| `--metrics-interval-ms` | Metrics polling interval; zero disables | 1000 |

Maximum logical concurrency is `processes × threads × queue depth`. Within a process, `threads × queue depth` must fit the transport's 16-bit send-queue capacity. Near the end of a run the window drains naturally. Poll mode consumes CPU continuously; wait mode sleeps only when its completion queue is empty.

Warmup and Get keys use `bench_key_<64 lowercase hex characters>_<index>`. Measured Put keys use
`bench_<64 lowercase hex characters>_p<rank>_t<thread>_<counter>`. All keys share the fixed hex segment
`2e3f71ee82c2e0f4c0e5dbe107b92d368cc34dc724c15dee5862abec4d25f437`, which resembles SHA-256 text.
Before warmup and the profiling/start barrier, each client precomputes all read keys and one complete
Put prefix per worker. During the measured workload, Gets copy cached warmup keys and Puts append only
the decimal counter to their cached worker prefix. The rank/thread/counter suffix keeps each Put key unique.
No hashing or hex generation is performed.

A logical Put is Alloc followed by Update, or BatchAlloc followed by BatchUpdate. Its slot remains occupied across both stages. Successful entries from partial batch allocation are updated, but any failed allocation or update makes the logical Put fail. Get performs a metadata RPC only. No data copy is timed or performed.

During the workload, a queue-depth table refreshes in place every second using ANSI cursor controls,
with one fixed row per client process and one
column per generator worker. Values are instantaneous pending RPC counts (up to `--queue-depth`),
not averages or transport send-queue occupancy. A Put retains its logical slot across Alloc and Update;
the transition may briefly sample as one fewer pending RPC. The table also prints at workload start and end.
Publishing counters and printing snapshots add a small amount of monitoring overhead.

## Timing and results

Each RPC captures steady-clock timestamps:

- **T0:** immediately before `MFAccApi::NonBlockSend()`, after construction, serialization, and pending registration.
- **T1:** immediately after submission returns.
- **T2:** at entry to the direct MemFabric response callback, before the benchmark copies the temporary response buffer.

**Submission duration** is T1−T0. **Client-observed RPC round-trip latency** is T2−T0. These overlap: do not add them, or subtract submission duration to claim server-only timing. A response may arrive before submission returns.

RPC timing includes submission internals, transport queues and scheduling, network time, and server queues and processing. It excludes benchmark request construction, response copying/deserialization, completion-queue handling, and work between Alloc and Update. Client overhead still affects achieved throughput.

The report prints one RPC round-trip latency table and one submission duration table for all client processes.
Each table has rows for `put-alloc`, `put-update`, and `get`, with columns for Samples, Avg (us), P50 (us),
P95 (us), P99 (us), P999 (us), Min (us), and Max (us). Latencies use microseconds and display two decimal places.
Each row pools observations from single and batch RPCs across all client threads and processes. Logical Put counts
and per-RPC outcomes appear below the tables. Each percentile is the exact observation at the one-based rank
`ceil(p × Samples)` in the pooled distribution. Samples counts RPC timing observations.

The startup configuration section shows the executable's build mode and the effective MetaService LRU type,
LRU shard count, async request workers, and TCP transport workers on separate lines. The runner stores them in
MongoDB as `build_mode`, `meta_lru_type`, `meta_lru_shard_count`, `meta_net_async_workers`, and
`meta_net_tcp_workers`. Plain LRU shows `N/A` for shards and stores that field as `null`.

For each run, the runner prints the full Git HEAD commit from that benchmark target's configured `cwd`
and stores it in MongoDB as `git_commit`. If Git cannot resolve HEAD, the runner prints `N/A` and
stores `null`.

The final report shows the mean and population standard deviation across MetaService workers in each pool
(`net_pool<N>` for MemCache async requests and `AccWrk:<N>` for TCP transport). The coordinator takes
per-thread Linux `/proc` snapshots after warmup, immediately before it releases the workload, and after all
clients drain, before teardown. CPU, user-space CPU, kernel CPU, runnable wait, and estimated sleep are
percentages of elapsed wall time per worker, with standard deviations in percentage points (pp). Voluntary and
involuntary context switches are counts per worker over that interval. The table displays every average and
standard deviation to two decimal places. The sleep estimate subtracts on-CPU runtime and run-queue wait from
elapsed time, with a floor of zero. CPU tick
granularity and scheduler counters that update at scheduling boundaries can affect short runs. The Worker count
row counts threads observed at both snapshots, and `(n=...)` shows when a metric uses fewer threads. Counts use
comma separators for thousands. Unavailable measurements, including wait and sleep on kernels without usable
per-thread scheduler counters, show as `N/A`. With MongoDB enabled the runner stores these values under
`meta_net_worker_stats`, keyed by pool and metric. Each metric contains numeric `workers`, `avg`, and `stddev`
fields, with `null` for unavailable averages and standard deviations.

Recording stores original `double` microsecond measurements in 64 power-of-two buckets, with capacity for at least
1,024 observations reserved per bucket before measurement (3 MiB of sample capacity per generator thread across
three operations and two timing distributions). Buckets grow as needed. After every client drains and profiling stops,
each client merges its thread samples and writes the buckets to an anonymous memory-backed file.
The coordinator merges those buckets and sorts only the buckets containing requested percentile ranks.
Transfer, merging, and sorting take place outside workload timing and the built-in profiling window. Memory grows
with the number of recorded RPCs, and vector growth during recording can affect performance. P999 means P99.9.
With fewer than 1,000 samples it is the maximum.

Success requires valid response structure and successful application statuses, including all requested entries for batches. Send failures and 30-second RPC timeouts count as failures but contribute no round-trip samples. Submission calls that returned contribute submission samples. Late and duplicate completions are discarded. A broken workload connection aborts the run; requests are not automatically retried.

Logical totals and achieved logical operations/sec count Put and Get only; Alloc and Update detail rows do not increase those totals. Failed logical operations are included in the achieved operation rate. Initialization and warmup are outside each process's measured interval. Throughput uses the longest client process duration, as in the original benchmark.
