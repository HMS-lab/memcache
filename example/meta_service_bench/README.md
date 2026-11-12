# meta_service Performance Benchmark

## Introduction

This benchmark measures **meta_service metadata processing performance**. It runs meta_service and simulated local_service clients in separate child processes, allowing concurrent access from multiple processes to measure metadata processing performance under load.

The benchmark does not use a real local_service. Each client child process simulates local_service behavior by creating a `MetaNetClient` directly, registering mock BM resources with fake GVA addresses and capacities, and sending RPC requests to meta_service. All BM data read and write operations are stubbed out.

### Multi-Process Architecture

```text
┌─────────────────────────────────────────────────────────────┐
│                    Coordinator (main)                       │
│  1. Fork the meta_service child process                      │
│  2. Wait for meta_service to become ready                    │
│  3. Fork N client child processes                            │
│  4. Wait for all clients to finish                           │
│  5. Collect results from shared memory                       │
│  6. Stop meta_service and print the aggregated report         │
├───────────────────┬───────────────────┬─────────────────────┤
│ meta_service      │ client[0]         │ client[N-1]         │
│ (child process)   │ (child process)   │ (child process)     │
│                   │ MetaNetClient     │ MetaNetClient       │
│ MmcMetaService    │ + mock BM         │ + mock BM           │
│ (in-process)      │ + M threads       │ + M threads         │
└───────────────────┴───────────────────┴─────────────────────┘
```

**Key design points:**

- meta_service runs in a separate child process created with `fork()`.
- Each client child process creates its own `MetaNetClient` and registers mock BM resources with a unique rank.
- Results from each process are collected through shared memory allocated with `mmap(MAP_SHARED | MAP_ANONYMOUS)`.
- The BM library is not initialized, and no NPU hardware is required.
- All BM data copy operations are skipped, so only metadata processing on the meta_service side is measured.

### Operations Tested

| Single Operations | Batch Operations |
|---------|---------|
| AllocRequest → AllocResponse | BatchAllocRequest → BatchAllocResponse |
| UpdateRequest → Response | BatchGetRequest → BatchAllocResponse |
| GetRequest → AllocResponse | BatchUpdateRequest → BatchUpdateResponse |
| QueryRequest → QueryResponse | BatchQueryRequest → BatchQueryResponse |
| IsExistRequest → IsExistResponse | BatchIsExistRequest → BatchIsExistResponse |
| RemoveRequest → Response | BatchRemoveRequest → BatchRemoveResponse |

## Prerequisites

### 1. Build and Install memcache

Run the following command from the memcache source root:

```bash
bash script/build_and_pack_run.sh --build_mode DEBUG --incremental
```

Build artifacts are placed in the `output/` directory.

### 2. Install the memfabric_hybrid .run Package

```bash
bash memfabric_hybrid-*.run          # Install memfabric_hybrid
source /usr/local/memfabric_hybrid/set_env.sh
```

## Building

### Method 1: Build Within the Source Tree (Recommended)

From the memcache source root, enable the benchmark build with `BUILD_CPP_EXAMPLE=ON`:

```bash
# 1. Configure CMake
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=DEBUG \
    -DBUILD_CPP_EXAMPLE=ON \
    -S . -B build/

# 2. Build the benchmark target
ninja -C build/ bench_meta_service
```

The executable is generated at `build/example/meta_service_bench/bench_meta_service`.

### Method 2: Standalone Build

After installing the memcache and memfabric_hybrid .run packages, you can build the benchmark independently in this directory:

```bash
# Set environment variables
export MEMCACHE_HYBRID_HOME_PATH=/path/to/memcache    # Source root
source /usr/local/memfabric_hybrid/set_env.sh

cd example/meta_service_bench
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The executable is generated at `example/meta_service_bench/build/bench_meta_service`.

> **Note:** The standalone build links against the shared library `libmf_memcache.so`, which must export internal C++ symbols. Building within the source tree links against the static library `mmc_static`, which provides all required symbols, and is recommended.

## Running

```bash
# Set the shared library search path (for a build within the source tree)
export LD_LIBRARY_PATH=$(pwd)/build/3rdparty/spdlog/lib:$(pwd)/build/3rdparty/spdlog/lib64:$LD_LIBRARY_PATH

# Run the benchmark
./build/example/meta_service_bench/bench_meta_service
```

### Command-Line Options

```text
Usage: bench_meta_service [options]
Options:
  -p <processes>    Client process count          (default: 4)
  -t <threads>      Threads per process           (default: 4)
  -n <ops>          Operations per thread          (default: 5000)
  -k <keyspace>     Key space size                (default: 20000)
  -w <warmup>       Warmup key count              (default: 5000)
  -b <batchsize>    Batch size for batch ops      (default: 32)
  -s <blobsize>     Blob size in bytes            (default: 32768)
  --perf             Enable perf profiling of meta_service   (default: off)
  --perf-freq <hz>   perf record sample frequency             (default: 99)
```

| Option | Description | Default |
|------|------|--------|
| `-p` | Number of client processes (each process has a unique rank) | 4 |
| `-t` | Number of worker threads per process | 4 |
| `-n` | Number of operations per thread | 5000 |
| `-k` | Key space size (all processes share the same key space) | 20000 |
| `-w` | Total number of warmup keys | 5000 |
| `-b` | Number of keys per batch operation | 32 |
| `-s` | Size of each blob in bytes | 32768 (32KB) |
| `--perf` | Enable perf sampling for the meta_service child process (attach mode); output is always written to `perf.data` | Off |
| `--perf-freq` | perf sampling frequency (Hz) | 99 |

### Examples

```bash
# Quick check with 2 processes x 2 threads
./build/example/meta_service_bench/bench_meta_service -p 2 -t 2 -n 500 -k 2000 -w 500

# Large-scale load test with 8 processes x 4 threads (32 concurrent workers)
./build/example/meta_service_bench/bench_meta_service -p 8 -t 4 -n 10000 -k 50000 -w 20000

# Collect perf data for meta_service, then analyze it with perf report after the load test
./build/example/meta_service_bench/bench_meta_service -p 4 -t 4 -n 5000 --perf --perf-freq 999
perf report -i perf.data

# Show help
./build/example/meta_service_bench/bench_meta_service -h
```

## Understanding the Output

The benchmark runs in four stages:

1. **Start meta_service:** Start `MmcMetaService` within its process and listen on a TCP port.
2. **Create simulated local_service clients:** Create `MetaNetClient` instances and connect to meta_service.
3. **Register mock BM resources:** Send `BmRegisterRequest` to register fake HBM resources (1 GB).
4. **Warm up and run the load test:** Warm up keys with Alloc+Update, then run mixed read/write and batch operations across multiple threads.

### Sample Output

```text
============================================================
  MetaService Multi-Process Benchmark
============================================================
Configuration:
  Processes:    2
  Threads/proc: 2
  Ops/thread:   200
  Total ops:    800
  Key space:    1000
  Blob size:    32768 bytes
  Warmup keys:  200
  Mock HBM/proc:1024 MB
  Workload:     Alloc=10% Update=10% Get=30% Query=20% IsExist=20% Remove=10%

[1/3] Starting meta_service process...
meta_service started (pid=47655)

[2/3] Forking 2 client processes...

[3/3] Waiting for 2 clients to finish...

========== Multi-Process Aggregated Report ==========

Processes: 2, Threads/proc: 2, Ops/thread: 200, Total ops: 800

+------------------------------------------------------------------------------------------------------------------------+
| Op         |  Success |     Fail |    Avg(us) |    P50(us) |    P95(us) |    P99(us) |    Min(us) |    Max(us) |
+------------------------------------------------------------------------------------------------------------------------+
| Alloc      |       56 |        0 |     175.46 |     170.00 |     204.00 |     257.00 |     143.00 |     302.00 |
| Update     |       88 |        0 |     153.48 |     120.00 |     162.50 |    1430.00 |     101.00 |    2671.00 |
| Get        |      260 |        0 |     147.02 |     125.00 |     169.50 |     230.50 |     100.00 |    2600.00 |
| Query      |      148 |        0 |     234.93 |     115.00 |     145.00 |    7662.00 |      98.00 |    7662.00 |
| IsExist    |      170 |        0 |     113.66 |     111.00 |     134.00 |     178.50 |      94.00 |     185.00 |
| Remove     |       78 |        0 |     320.47 |     122.50 |     185.50 |    7713.50 |      99.00 |    7714.00 |
+------------------------------------------------------------------------------------------------------------------------+

Total ops: 800 (success: 800, failure: 0)
Max duration (wall): 0.04 sec
Throughput:         22317.69 ops/sec

Per-process summary:
  Rank   |      PID |      Ops |     Succ |     Fail |   Duration
  0      |    47733 |      400 |      400 |        0 |       0.04
  1      |    47734 |      400 |      400 |        0 |       0.04

Benchmark finished.
```

### Metrics

| Metric | Description |
|------|------|
| Success / Fail | Number of successful/failed RPC calls (a return value of `MMC_OK` indicates success) |
| Avg(us) | Average latency in microseconds |
| P50(us) | Median latency (50th percentile) |
| P95(us) | 95th-percentile latency |
| P99(us) | 99th-percentile latency |
| Min/Max(us) | Minimum/maximum latency |
| Throughput | Total throughput (ops/sec) |

### Workload Distribution

**Mixed reads and writes (single operations):**

| Operation | Share | Description |
|------|------|------|
| Alloc | 10% | Request memory from meta_service (allocator management + LRU insertion) |
| Update | 10% | Update the blob status to indicate that it has been written |
| Get | 30% | Retrieve blob metadata (triggers lease renewal) |
| Query | 20% | Query metadata for a key |
| IsExist | 20% | Check whether a key exists |
| Remove | 10% | Delete metadata and free memory (allocator reclamation + LRU removal) |

**Batch operations:** Each round runs BatchAlloc / BatchGet / BatchUpdate / BatchQuery / BatchIsExist / BatchRemove in sequence, with `-b` keys per batch.
