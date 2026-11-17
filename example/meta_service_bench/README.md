# MetaService metadata benchmark

This tool runs MetaService and simulated local-service clients in separate child processes. It measures metadata RPC performance without device memory transfers or NPU hardware. Mock BM registration supplies fake HBM capacity; backup requests are acknowledged without copying cached data.

Each client process retains its `MetaNetClient` connection for registration, warmup, and server callbacks. A separate MemFabric connection submits measured requests asynchronously. Its handshake uses an invalid rank so it does not replace the registered connection. Request messages carry the registered rank where required.

All processes finish registration, validated warmup, and async connection setup before the coordinator releases the startup barrier. Once a process drains its workload, it stops its timer and waits at the teardown barrier with its clients connected. Clients disconnect only after every process has drained its requests, including dependent Updates. Both barriers are outside workload measurement. Setup errors, failed warmup, or a client exiting before a barrier completes abort the run and release waiting peers.

## Build and verify

From the repository root, use the supported build entrypoint:

```bash
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental --build_cpp_example ON
./build/example/meta_service_bench/bench_meta_service_async_test
./build/example/meta_service_bench/bench_meta_service_barrier_test
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

A logical Put is Alloc followed by Update, or BatchAlloc followed by BatchUpdate. Its slot remains occupied across both stages. Successful entries from partial batch allocation are updated, but any failed allocation or update makes the logical Put fail. Get performs a metadata RPC only. No data copy is timed or performed.

## Timing and results

Each RPC captures steady-clock timestamps:

- **T0:** immediately before `MFAccApi::NonBlockSend()`, after construction, serialization, and pending registration.
- **T1:** immediately after submission returns.
- **T2:** at entry to the direct MemFabric response callback, before the benchmark copies the temporary response buffer.

**Submission duration** is T1−T0. **Client-observed RPC round-trip latency** is T2−T0. These overlap: do not add them, or subtract submission duration to claim server-only timing. A response may arrive before submission returns.

RPC timing includes submission internals, transport queues and scheduling, network time, and server queues and processing. It excludes benchmark request construction, response copying/deserialization, completion-queue handling, and work between Alloc and Update. Client overhead still affects achieved throughput.

The report prints two aligned latency tables, each with exactly three rows: `put-alloc`, `put-update`, and `get`. The first table shows RPC round-trip latency and the second shows submission duration. Both contain Samples, Avg (us), P50 (us), P95 (us), P99 (us), P999 (us), Min (us), and Max (us); `us` means microseconds. Values have two decimal places. Rows combine single and batch RPCs. Logical Put counts and per-RPC outcomes are printed separately; there is no combined Put latency. Percentiles use merged power-of-two histogram buckets and are approximate upper bounds.

Success requires valid response structure and successful application statuses, including all requested entries for batches. Send failures and 30-second RPC timeouts count as failures but contribute no round-trip samples. Submission calls that returned contribute submission samples. Late and duplicate completions are discarded. A broken workload connection aborts the run; requests are not automatically retried.

Logical totals and achieved logical operations/sec count Put and Get only; Alloc and Update detail rows do not increase those totals. Failed logical operations are included in the achieved operation rate. Initialization and warmup are outside each process's measured interval. Throughput uses the longest client process duration, as in the original benchmark.
