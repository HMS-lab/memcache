#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
"""
batch_get_key_info latency benchmark — multi-process query.

Writes a large dataset in batches (concurrent), then spawns multiple query
worker processes (each with init_bm=False, pure client) to measure per-call
latency of store.batch_get_key_info() concurrently.
Reports per-worker and aggregated avg / p50 / p99 / per-key / throughput.

Prerequisites:
  1. Start MetaService:
     export MMC_META_CONFIG_PATH=/path/to/config/mmc-meta.conf
     python -c "from memcache_hybrid import MetaService; MetaService.main()" &

  2. Set environment:
     export MMC_LOCAL_CONFIG_PATH=/path/to/config/mmc-local.conf
     source /usr/local/Ascend/ascend-toolkit/set_env.sh
     source /usr/local/memcache_hybrid/set_env.sh

Usage:
  python example/benchmark/bench_batch_get_key_info.py
  python example/benchmark/bench_batch_get_key_info.py --query_workers 8
  python example/benchmark/bench_batch_get_key_info.py --flag 0   # plain query
  python example/benchmark/bench_batch_get_key_info.py --flag 1   # layer-wise (default)
"""

import argparse
import multiprocessing as mp
import os
import statistics
import time
from concurrent.futures import ThreadPoolExecutor, as_completed


def set_device(device_id):
    import acl

    acl.init()
    ret = acl.rt.set_device(device_id)
    if ret != 0:
        raise RuntimeError(f"acl set device {device_id} failed, ret={ret}")


def write_batch(store, keys, key_size, write_buffer_ptr):
    """batch_alloc + batch_copy(write) + batch_write_finish for one batch of keys."""
    from memcache_hybrid import L2G

    sizes = [key_size] * len(keys)
    buffer_ptrs = [write_buffer_ptr] * len(keys)

    lease_ttl_ms = 300000
    gvas = store.batch_alloc(keys, sizes, 1, lease_ttl_ms)
    store.batch_copy(gvas, buffer_ptrs, sizes, L2G)

    results = store.batch_write_finish(keys, [0] * len(keys))
    if any(r != 0 for r in results):
        raise RuntimeError(f"batch_write_finish failed: {results}")


def write_phase(store, total_keys, write_batch_size, key_size, write_workers):
    """Write total_keys in batches of write_batch_size using a thread pool.

    Returns list of key batches (in original order)."""
    import torch
    import torch_npu

    num_batches = (total_keys + write_batch_size - 1) // write_batch_size
    key_batches = []
    for batch_idx in range(num_batches):
        start = batch_idx * write_batch_size
        end = min(start + write_batch_size, total_keys)
        keys = [f"bench_q_{i}" for i in range(start, end)]
        key_batches.append(keys)

    write_buffer = torch.randint(0, 256, (key_size,), dtype=torch.uint8, device="npu")
    write_buffer_ptr = write_buffer.data_ptr()

    completed = 0
    with ThreadPoolExecutor(max_workers=write_workers) as pool:
        futures = {
            pool.submit(write_batch, store, key_batches[i], key_size, write_buffer_ptr): i for i in range(num_batches)
        }
        for future in as_completed(futures):
            future.result()
            completed += 1
            if completed % 100 == 0 or completed == num_batches:
                print(f"  write batch {completed}/{num_batches} done")

    torch.npu.current_stream().synchronize()
    return key_batches


def query_worker(worker_id, key_batches, query_batch_size, iterations, warmup, flag, sync, result_queue):
    """Query worker process: init with init_bm=False, measure batch_get_key_info latency."""
    from memcache_hybrid import DistributedObjectStore

    store = DistributedObjectStore()
    ret = store.init(0, init_bm=False)
    if ret != 0:
        result_queue.put((worker_id, RuntimeError(f"store.init failed: {ret}")))
        return

    num_batches = len(key_batches)

    for w in range(warmup):
        keys = key_batches[w % num_batches][:query_batch_size]
        store.batch_get_key_info(keys, flag=flag)

    sync.wait()

    latencies = []
    t_start = time.perf_counter()
    for i in range(iterations):
        keys = key_batches[i % num_batches][:query_batch_size]
        t0 = time.perf_counter()
        store.batch_get_key_info(keys, flag=flag)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1e6)
    t_end = time.perf_counter()

    sync.wait()

    store.close()
    wall_us = (t_end - t_start) * 1e6
    result_queue.put((worker_id, latencies, wall_us))


def percentile(sorted_data, pct):
    idx = min(int(len(sorted_data) * pct / 100), len(sorted_data) - 1)
    return sorted_data[idx]


def main():
    parser = argparse.ArgumentParser(description="batch_get_key_info latency benchmark")
    parser.add_argument("--device_id", type=int, default=0, help="NPU device id for write phase")
    parser.add_argument("--total_keys", type=int, default=1000000, help="Total keys to write")
    parser.add_argument("--write_batch", type=int, default=1000, help="Keys per write batch")
    parser.add_argument("--query_batch", type=int, default=1000, help="Keys per query call")
    parser.add_argument("--write_workers", type=int, default=4, help="Concurrent write threads")
    parser.add_argument("--query_workers", type=int, default=8, help="Concurrent query processes")
    parser.add_argument("--iterations", type=int, default=100, help="Timed query iterations per worker")
    parser.add_argument("--warmup", type=int, default=10, help="Warmup rounds per worker (not counted)")
    parser.add_argument("--key_size", type=int, default=4096, help="Data size per key in bytes")
    parser.add_argument("--flag", type=int, default=1, help="Query flag: 1=layer-wise, 0=plain")
    args = parser.parse_args()

    flag_name = "GET_KEY_INFO_FOR_LAYER_WISE" if args.flag == 1 else "PLAIN"
    print(f"=== batch_get_key_info Latency Benchmark (Multi-Process) ===")
    print(
        f"device_id={args.device_id}, total_keys={args.total_keys}, "
        f"write_batch={args.write_batch}, query_batch={args.query_batch}"
    )
    print(f"write_workers={args.write_workers}, query_workers={args.query_workers}, key_size={args.key_size}")
    print(f"flag={args.flag}({flag_name}), iterations={args.iterations}\n")

    print("Write phase:")
    set_device(args.device_id)
    from memcache_hybrid import DistributedObjectStore

    store = DistributedObjectStore()
    ret = store.init(args.device_id, init_bm=True)
    if ret != 0:
        raise RuntimeError(f"store.init failed, ret={ret}")

    t_write_start = time.perf_counter()
    key_batches = write_phase(store, args.total_keys, args.write_batch, args.key_size, args.write_workers)
    t_write_end = time.perf_counter()
    print(f"  total {args.total_keys} keys written in {(t_write_end - t_write_start):.2f}s\n")

    print(
        f"Query phase: {args.query_workers} workers x {args.query_batch} keys/call "
        f"x {args.iterations} iterations (init_bm=False)"
    )

    ctx = mp.get_context('spawn')
    sync = ctx.Barrier(args.query_workers)
    result_queue = ctx.Queue()

    processes = []
    for i in range(args.query_workers):
        p = ctx.Process(
            target=query_worker,
            args=(i, key_batches, args.query_batch, args.iterations, args.warmup, args.flag, sync, result_queue),
        )
        p.start()
        processes.append(p)

    for p in processes:
        p.join()

    per_worker = {}
    all_latencies = []
    worker_wall_us = {}
    errors = []
    while not result_queue.empty():
        item = result_queue.get()
        worker_id = item[0]
        if isinstance(item[1], Exception):
            errors.append((worker_id, item[1]))
        else:
            per_worker[worker_id] = item[1]
            worker_wall_us[worker_id] = item[2]
            all_latencies.extend(item[1])

    if errors:
        for wid, err in errors:
            print(f"  Worker {wid} ERROR: {err}")
        return

    print(f"\nPer-Worker:")
    for wid in sorted(per_worker.keys()):
        lats = per_worker[wid]
        w_avg = statistics.mean(lats)
        w_p50 = statistics.median(lats)
        w_p99 = percentile(sorted(lats), 99)
        w_wall = worker_wall_us[wid]
        print(f"  Worker {wid}: avg={w_avg:.2f}us, p50={w_p50:.2f}us, p99={w_p99:.2f}us, wall={w_wall / 1000:.2f}ms")

    total_calls = len(all_latencies)
    avg = statistics.mean(all_latencies)
    p50 = statistics.median(all_latencies)
    sorted_lat = sorted(all_latencies)
    p99 = percentile(sorted_lat, 99)
    per_key = avg / args.query_batch
    total_ms = sum(all_latencies) / 1000
    max_wall_ms = max(worker_wall_us.values()) / 1000
    throughput = total_calls / (max_wall_ms / 1000) if max_wall_ms > 0 else 0

    print(f"\nAggregated ({total_calls} calls, {args.query_workers} workers):")
    print(f"{'Metric':<12} | {'Value':>10}")
    print("-" * 28)
    print(f"{'Avg':<12} | {avg:>8.2f} us")
    print(f"{'P50':<12} | {p50:>8.2f} us")
    print(f"{'P99':<12} | {p99:>8.2f} us")
    print(f"{'Per-Key':<12} | {per_key:>8.4f} us")
    print(f"{'Wall Time':<12} | {max_wall_ms:>8.2f} ms")
    print(f"{'Total CPU':<12} | {total_ms:>8.2f} ms")
    print(f"{'Throughput':<12} | {throughput:>8.2f} calls/s")

    store.close()
    print("\nDone.")


if __name__ == "__main__":
    main()
