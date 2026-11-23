#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import hashlib
import importlib.util
import math
import os
import random
import subprocess
import sys
import sysconfig
from pathlib import Path
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


def _load_native_module():
    script_dir = Path(__file__).resolve().parent
    identity = hashlib.sha256(
        f"{script_dir}:{sys.executable}:{sys.implementation.cache_tag}".encode()
    ).hexdigest()[:12]
    build_dir = script_dir / "build" / identity
    command = [
        "cmake", "-S", str(script_dir), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release", f"-DPython3_EXECUTABLE={sys.executable}",
        f"-DJSON_HEADERS={os.environ.get('BLOB_SIM_JSON_HEADERS', '')}",
    ]
    python_headers = os.environ.get("BLOB_SIM_PYTHON_HEADERS")
    if python_headers:
        command.append(f"-DPython3_INCLUDE_DIR={python_headers}")
    subprocess.run(command, check=True, stdout=sys.stderr)
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "_allocator_sim_native", "--parallel", "2"],
        check=True, stdout=sys.stderr,
    )
    path = build_dir / "native" / ("_allocator_sim_native" + sysconfig.get_config_var("EXT_SUFFIX"))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    existing = sys.modules.get("_allocator_sim_native")
    if existing is not None:
        if existing.__binary_digest__ != digest:
            raise RuntimeError("Native code changed. Start a new Python process before measurement.")
        return existing
    spec = importlib.util.spec_from_file_location("_allocator_sim_native", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.__binary_digest__ = digest
    sys.modules[spec.name] = module
    return module


PERCENT = 100.0

UNIT_MULTIPLIERS = {
    "b": 1,
    "kib": 1024,
    "kb": 1024,
    "k": 1024,
    "mib": 1024 ** 2,
    "mb": 1024 ** 2,
    "m": 1024 ** 2,
    "gib": 1024 ** 3,
    "gb": 1024 ** 3,
    "g": 1024 ** 3,
    "tib": 1024 ** 4,
    "tb": 1024 ** 4,
    "t": 1024 ** 4,
}


def parse_size(text: str) -> int:
    s = text.strip().lower().replace("_", "")
    num_part = ""
    unit_part = ""
    for ch in s:
        if ch.isdigit() or ch == ".":
            if unit_part:
                raise ValueError(f"Invalid size format: {text}")
            num_part += ch
        elif not ch.isspace():
            unit_part += ch
    if not num_part:
        raise ValueError(f"Invalid size value: {text}")
    val = float(num_part)
    if not unit_part:
        return int(val)
    if unit_part not in UNIT_MULTIPLIERS:
        raise ValueError(f"Unknown unit '{unit_part}' in '{text}'")
    return int(round(val * UNIT_MULTIPLIERS[unit_part]))


def format_human(num_bytes: int) -> str:
    if num_bytes == 0:
        return "0 B"
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    idx = 0
    val = float(num_bytes)
    while val >= 1024.0 and idx < len(units) - 1:
        val /= 1024.0
        idx += 1
    if idx == 0:
        return f"{num_bytes} B"
    return f"{val:.2f} {units[idx]}"


def format_bytes(num_bytes: int) -> str:
    return f"{num_bytes:,} bytes ({format_human(num_bytes)})"


def parse_block_sizes(spec: str) -> Tuple[List[int], List[float]]:
    sizes: List[int] = []
    weights: List[float] = []
    parts = [p.strip() for p in spec.split(",") if p.strip()]
    if not parts:
        raise ValueError("Empty --block-sizes specification")
    for part in parts:
        if ":" in part:
            sz_str, w_str = part.split(":", 1)
            w_clean = w_str.strip().rstrip("%")
            weight = float(w_clean)
        else:
            sz_str = part
            weight = 1.0
        sz = parse_size(sz_str)
        if sz <= 0 or sz > (1 << 64) - 4096:
            raise ValueError(f"Block size must be > 0: {part}")
        if not math.isfinite(weight) or weight <= 0:
            raise ValueError("Block weights must be finite and positive.")
        sizes.append(sz)
        weights.append(weight)
    total_w = sum(weights)
    if total_w <= 0:
        raise ValueError("Sum of weights must be > 0")
    norm_weights = [w * 100.0 / total_w for w in weights]
    return sizes, norm_weights


@dataclass
class SizeStats:
    size: int
    attempts: int = 0
    successes: int = 0
    capacity_failures: int = 0
    fragmentation_failures: int = 0
    all_bytes: int = 0
    success_bytes: int = 0
    failed_bytes: int = 0


@dataclass
class LiveBlock:
    handle: int
    size: int
    reserved_size: int
    is_hot: bool


class LiveBlocks:
    def __init__(self, policy: str, rng: random.Random, evict_rng: Optional[random.Random] = None):
        self.policy = policy.lower()
        self.rng = rng
        self.evict_rng = evict_rng if evict_rng is not None else rng
        self.blocks: "OrderedDict[int, LiveBlock]" = OrderedDict()
        self.hot_ids: List[int] = []
        self.cold_ids: List[int] = []
        self.hot_count = 0
        self.cold_count = 0

    def __len__(self) -> int:
        return len(self.blocks)

    def add(self, block: LiveBlock) -> None:
        self.blocks[block.handle] = block
        if block.is_hot:
            self.hot_count += 1
            self.hot_ids.append(block.handle)
        else:
            self.cold_count += 1
            self.cold_ids.append(block.handle)

    def read_one(self, prefer_hot: bool) -> Optional[str]:
        if not self.blocks:
            return None
        if prefer_hot:
            pools = (self.hot_ids, self.cold_ids) if self.hot_count > 0 else (self.cold_ids, self.hot_ids)
        else:
            pools = (self.cold_ids, self.hot_ids) if self.cold_count > 0 else (self.hot_ids, self.cold_ids)
        for pool in pools:
            while pool:
                idx = self.rng.randrange(len(pool))
                h = pool[idx]
                blk = self.blocks.get(h)
                if blk is None:
                    pool[idx] = pool[-1]
                    pool.pop()
                    continue
                if self.policy == "lru":
                    self.blocks.move_to_end(h, last=True)
                return "hot" if blk.is_hot else "cold"
        return None

    def pop_victim(self) -> Optional[LiveBlock]:
        if not self.blocks:
            return None
        if self.policy in ("lru", "fifo"):
            _, blk = self.blocks.popitem(last=False)
        else:
            keys = list(self.blocks.keys())
            victim_key = self.evict_rng.choice(keys)
            blk = self.blocks.pop(victim_key)
        if blk.is_hot:
            self.hot_count -= 1
        else:
            self.cold_count -= 1
        return blk


def build_histogram_buckets(max_capacity: int) -> List[Tuple[int, int, str]]:
    buckets = []
    upper = 4096
    lower = 0
    while lower < max_capacity or upper <= 2 * 1024 * 1024 * 1024:
        label = f"({format_human(lower)}, {format_human(upper)}]"
        buckets.append((lower, upper, label))
        lower = upper
        upper *= 2
        if lower >= max(max_capacity, 2 * 1024 * 1024 * 1024):
            break
    return buckets


def compute_histogram(
    ranges: List[Tuple[int, int]], buckets: List[Tuple[int, int, str]]
) -> List[Tuple[str, int, int]]:
    counts = [0] * len(buckets)
    bytes_sum = [0] * len(buckets)
    for _, sz in ranges:
        if sz <= 0 or sz > (1 << 64) - 4096:
            continue
        if sz <= 4096:
            idx = 0
        else:
            idx = int(math.ceil(math.log2(sz / 4096.0)))
            if idx < 0:
                idx = 0
            elif idx >= len(buckets):
                idx = len(buckets) - 1
        counts[idx] += 1
        bytes_sum[idx] += sz
    return [(buckets[i][2], counts[i], bytes_sum[i]) for i in range(len(buckets))]


def format_slab_summary(slab_stats: dict) -> str:
    lines = []
    lines.append("Slab Allocator Frame & Pool Summary:")
    lines.append(f"  Frame size       : {format_bytes(slab_stats['frame_size'])}")
    lines.append(
        f"  Free full frames : {slab_stats['free_full_frames']:,} "
        f"({format_bytes(slab_stats['free_full_frame_bytes'])})"
    )
    if slab_stats.get("free_tail_frame_bytes", 0) > 0:
        lines.append(f"  Free tail frame  : 1 ({format_bytes(slab_stats['free_tail_frame_bytes'])})")
    else:
        lines.append("  Free tail frame  : 0 (0 bytes (0 B))")
    lines.append("")

    header = (
        f"{'Slot size':<30}  {'Frames':>6}  {'Used slots':>12}  "
        f"{'Free slots':>12}  {'Free space in pool':>30}  {'Frame tail waste':>26}"
    )
    lines.append(header)
    lines.append("-" * len(header))

    pools = slab_stats.get("pools", [])
    if not pools:
        lines.append("  (no active slab pools)")
    else:
        for p in pools:
            slot_str = format_bytes(p["slot_size"])
            free_str = format_bytes(p["free_bytes"])
            waste_str = format_bytes(p["tail_waste_bytes"])
            lines.append(
                f"{slot_str:<30}  {p['active_frames']:>6,}  {p['allocated_slots']:>12,}  "
                f"{p['free_slots']:>12,}  {free_str:>30}  {waste_str:>26}"
            )
    return "\n".join(lines)


def run_simulation(args: argparse.Namespace) -> dict:
    capacity = parse_size(args.capacity)
    sizes, weights = parse_block_sizes(args.block_sizes)
    if capacity <= 0 or capacity > (1 << 64) - 4096:
        raise ValueError("Capacity must be positive and must not overflow alignment.")
    if args.steps <= 0 or args.sample_every <= 0 or args.reads_per_allocation < 0:
        raise ValueError("Steps and sample interval must be positive. Read count must not be negative.")
    if not 0 <= args.min_water_level < args.max_water_level <= PERCENT:
        raise ValueError("Water levels must satisfy 0 <= minimum < maximum <= 100.")
    if not all(0 <= value <= PERCENT for value in (args.hot_block_percent, args.hot_read_percent)):
        raise ValueError("Hot block and read percentages must be between 0 and 100.")
    workload_rng = random.Random(args.seed)
    read_rng = random.Random(args.seed + 1)
    evict_rng = random.Random(args.seed + 2)

    native = _load_native_module()
    sim = native.NativeAllocatorSim(capacity, args.mode)
    live = LiveBlocks(args.eviction_policy, read_rng, evict_rng)
    size_stats: Dict[int, SizeStats] = {sz: SizeStats(size=sz) for sz in sizes}

    counters = {
        "attempts": 0,
        "successes": 0,
        "capacity_failures": 0,
        "fragmentation_failures": 0,
        "releases": 0,
        "skipped_releases": 0,
        "eviction_cycles": 0,
        "evictions": 0,
        "evicted_reserved_bytes": 0,
        "reads": 0,
        "hot_reads": 0,
        "cold_reads": 0,
        "skipped_reads": 0,
        "hot_assignments": 0,
        "cold_assignments": 0,
    }

    high_bytes = int(capacity * (args.max_water_level / PERCENT))
    low_bytes = int(capacity * (args.min_water_level / PERCENT))

    samples = []
    progress_step = max(1, args.steps // 100)

    for step in range(1, args.steps + 1):
        # Sprawdzenie progu ewakuacji (water-level) z uwzględnieniem odpadu ogonowego ramek
        if sim.reserved_size() >= high_bytes and len(live) > 0:
            counters["eviction_cycles"] += 1
            while sim.reserved_size() > low_bytes and len(live) > 0:
                victim = live.pop_victim()
                if victim is None:
                    break
                if sim.release(victim.handle):
                    counters["releases"] += 1
                    counters["evictions"] += 1
                    counters["evicted_reserved_bytes"] += victim.reserved_size
                else:
                    counters["skipped_releases"] += 1

        # Odczyty symulujące ruch LRU
        for _ in range(args.reads_per_allocation):
            prefer_hot = (workload_rng.random() * PERCENT) < args.hot_read_percent
            read_kind = live.read_one(prefer_hot)
            if read_kind is None:
                counters["skipped_reads"] += 1
            else:
                counters["reads"] += 1
                if read_kind == "hot":
                    counters["hot_reads"] += 1
                else:
                    counters["cold_reads"] += 1

        # Losowanie rozmiaru i statusu hot (zawsze w tej samej kolejności niezależnie od sukcesu alokacji)
        req_size = workload_rng.choices(sizes, weights=weights, k=1)[0]
        is_hot = (workload_rng.random() * PERCENT) < args.hot_block_percent
        st = size_stats[req_size]
        st.attempts += 1
        st.all_bytes += req_size
        counters["attempts"] += 1

        res = sim.alloc(req_size)
        if res is not None:
            handle, _, alloc_sz = res
            if is_hot:
                counters["hot_assignments"] += 1
            else:
                counters["cold_assignments"] += 1
            live.add(LiveBlock(handle=handle, size=req_size, reserved_size=alloc_sz, is_hot=is_hot))
            st.successes += 1
            st.success_bytes += req_size
            counters["successes"] += 1
        else:
            free_space = max(0, capacity - sim.reserved_size())
            if free_space < req_size:
                counters["capacity_failures"] += 1
                st.capacity_failures += 1
            else:
                counters["fragmentation_failures"] += 1
                st.fragmentation_failures += 1
            st.failed_bytes += req_size

        if args.sample_every > 0 and (step % args.sample_every == 0):
            # W trybie slab nie pobieramy setek tysięcy wycinków w trakcie biegu
            snap = sim.snapshot(include_segments=(args.mode != "slab"))
            samples.append(snap)

        if step % progress_step == 0 or step == args.steps:
            pct = int(step * 100 / args.steps)
            sys.stdout.write(f"\rProgress: {pct}% ({step:,}/{args.steps:,} allocation attempts)")
            sys.stdout.flush()

    sys.stdout.write("\n")

    # Końcowy snapshot pamięci
    final_snap = sim.snapshot(include_segments=(args.mode != "slab"))
    cap = final_snap["capacity"]
    req = final_snap["requested"]
    res_bytes = final_snap["reserved"]
    free_b = final_snap["free"]
    int_frag = final_snap["internal_fragmentation"]
    largest_free = final_snap["largest_free_range"]

    occ_pct = (res_bytes * PERCENT / cap) if cap > 0 else 0.0
    int_frag_pct = (int_frag * PERCENT / res_bytes) if res_bytes > 0 else 0.0
    ext_frag_pct = ((free_b - largest_free) * PERCENT / free_b) if free_b > 0 else 0.0

    config_info = {
        "capacity": format_bytes(capacity),
        "eviction_policy": args.eviction_policy,
        "hot_block_percent": f"{int(args.hot_block_percent)}%",
        "hot_read_percent": f"{int(args.hot_read_percent)}%",
        "max_water_level": f"{int(args.max_water_level)}%",
        "min_water_level": f"{int(args.min_water_level)}%",
        "mode": args.mode,
        "reads_per_allocation": args.reads_per_allocation,
        "sample_every": args.sample_every,
        "seed": args.seed,
        "sizes": [format_bytes(sz) for sz in sizes],
        "steps": args.steps,
        "weights": [f"{format_human(sz)} : {w:.2f}%" for sz, w in zip(sizes, weights)],
        "workload": args.workload,
    }

    report = {
        "configuration": config_info,
        "counters": counters,
        "memory": {
            "capacity": cap,
            "requested": req,
            "reserved": res_bytes,
            "free": free_b,
            "internal_fragmentation": int_frag,
            "alignment_waste": final_snap["alignment_waste"],
            "max_value_align_padding": final_snap["max_value_align_padding"],
            "frame_tail_waste": final_snap["frame_tail_waste"],
            "largest_free_range": largest_free,
            "occupancy_percent": occ_pct,
            "free_ranges": final_snap["free_ranges_count"],
            "hot_live_blocks": live.hot_count,
            "cold_live_blocks": live.cold_count,
            "internal_fragmentation_percent": int_frag_pct,
            "external_fragmentation_percent": ext_frag_pct,
        },
        "size_stats": [
            {
                "size": sz,
                "attempts": size_stats[sz].attempts,
                "successes": size_stats[sz].successes,
                "capacity_failures": size_stats[sz].capacity_failures,
                "fragmentation_failures": size_stats[sz].fragmentation_failures,
                "all_bytes": size_stats[sz].all_bytes,
                "success_bytes": size_stats[sz].success_bytes,
                "failed_bytes": size_stats[sz].failed_bytes,
            }
            for sz in sizes
        ],
    }

    if args.mode == "slab":
        report["slab_summary"] = final_snap["slab_stats"]
    else:
        buckets = build_histogram_buckets(capacity)
        hist = compute_histogram(final_snap["free_ranges"], buckets)
        report["free_range_histogram"] = [
            {"range": label, "count": cnt, "bytes": b_sum} for label, cnt, b_sum in hist
        ]

    return report


def print_report(report: dict) -> None:
    cfg = report["configuration"]
    c = report["counters"]
    m = report["memory"]

    print("Configuration:")
    print(json.dumps(cfg, indent=2))
    print(f"\nMode: {cfg['mode']}")
    print(f"Eviction policy: {cfg['eviction_policy']}\n")

    print("Counters:")
    print(f"{'Counter':<22}  {'Value':>34}")
    print(f"{'-' * 22}  {'-' * 34}")
    rows = [
        ("attempts", f"{c['attempts']:,}"),
        ("successes", f"{c['successes']:,}"),
        ("capacity failures", f"{c['capacity_failures']:,}"),
        ("fragmentation failures", f"{c['fragmentation_failures']:,}"),
        ("releases", f"{c['releases']:,}"),
        ("skipped releases", f"{c['skipped_releases']:,}"),
        ("eviction cycles", f"{c['eviction_cycles']:,}"),
        ("evictions", f"{c['evictions']:,}"),
        ("evicted reserved bytes", format_bytes(c["evicted_reserved_bytes"])),
        ("reads", f"{c['reads']:,}"),
        ("hot reads", f"{c['hot_reads']:,}"),
        ("cold reads", f"{c['cold_reads']:,}"),
        ("skipped reads", f"{c['skipped_reads']:,}"),
        ("hot assignments", f"{c['hot_assignments']:,}"),
        ("cold assignments", f"{c['cold_assignments']:,}"),
    ]
    for name, val in rows:
        print(f"{name:<22}  {val:>34}")

    print("\nMemory:")
    print(f"{'Metric':<23}  {'Size':>34}")
    print(f"{'-' * 23}  {'-' * 34}")
    mem_rows = [
        ("Capacity", format_bytes(m["capacity"])),
        ("Requested", format_bytes(m["requested"])),
        ("Reserved", format_bytes(m["reserved"])),
        ("Free", format_bytes(m["free"])),
        ("Internal fragmentation", format_bytes(m["internal_fragmentation"])),
        ("Alignment waste", format_bytes(m["alignment_waste"])),
        ("Max-value-align padding", format_bytes(m["max_value_align_padding"])),
    ]
    if cfg["mode"] == "slab":
        mem_rows.append(("Frame tail waste", format_bytes(m["frame_tail_waste"])))
    mem_rows.append(("Largest free range", format_bytes(m["largest_free_range"])))

    for name, val in mem_rows:
        print(f"{name:<23}  {val:>34}")

    print(f"Occupancy: {m['occupancy_percent']:.2f}% (denominator: capacity bytes)")
    print(f"Free ranges: {m['free_ranges']:,}")
    print(f"Live blocks: {m['hot_live_blocks']:,} hot, {m['cold_live_blocks']:,} cold")
    print(f"Internal fragmentation: {m['internal_fragmentation_percent']:.2f}% (denominator: reserved bytes)")
    print(f"External fragmentation: {m['external_fragmentation_percent']:.2f}% (denominator: free bytes)\n")

    print("Allocation attempts by size:")
    print(
        f"{'Requested size':<30}  {'Attempts':>8}  {'Successes':>9}  "
        f"{'Capacity failures':>17}  {'Fragmentation failures':>22}"
    )
    print(f"{'-' * 30}  {'-' * 8}  {'-' * 9}  {'-' * 17}  {'-' * 22}")
    for st in report["size_stats"]:
        sz_str = format_bytes(st["size"])
        print(
            f"{sz_str:<30}  {st['attempts']:>8,}  {st['successes']:>9,}  "
            f"{st['capacity_failures']:>17,}  {st['fragmentation_failures']:>22,}"
        )

    print("\nAllocation data by size (cumulative requested bytes, excludes padding):")
    print(f"{'Requested size':<14}  {'All attempts':>12}  {'Successful':>10}  {'Failed':>10}")
    print(f"{'-' * 14}  {'-' * 12}  {'-' * 10}  {'-' * 10}")
    for st in report["size_stats"]:
        print(
            f"{format_human(st['size']):<14}  {format_human(st['all_bytes']):>12}  "
            f"{format_human(st['success_bytes']):>10}  {format_human(st['failed_bytes']):>10}"
        )
    print("")

    # W trybie slab drukujemy podsumowanie ramek i pul zamiast histogramu wycinków
    if cfg["mode"] == "slab" and "slab_summary" in report:
        print(format_slab_summary(report["slab_summary"]))
    elif "free_range_histogram" in report:
        print("Free-range histogram:")
        print(f"{'Range size (lower exclusive, upper inclusive)':<45}  {'Count':>6}  {'Actual free space':>30}")
        print(f"{'-' * 45}  {'-' * 6}  {'-' * 30}")
        for h in report["free_range_histogram"]:
            print(f"{h['range']:<45}  {h['count']:>6,}  {format_bytes(h['bytes']):>30}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Blob Allocator Simulator (Best-Fit & Slab)")
    parser.add_argument("--capacity", type=str, required=True, help="Total capacity, e.g. 50GiB, 100GiB")
    parser.add_argument(
        "--block-sizes",
        type=str,
        required=True,
        help="Comma-separated block sizes with optional weights, e.g. 4KiB:20%%,4MiB:80%%",
    )
    parser.add_argument("--steps", type=int, default=100000, help="Number of allocation attempts")
    parser.add_argument(
        "--mode",
        type=str,
        choices=["4k-align", "max-value-align", "slab"],
        default="4k-align",
        help="Allocator mode",
    )
    parser.add_argument(
        "--eviction-policy",
        type=str,
        choices=["lru", "fifo", "random"],
        default="lru",
        help="Eviction policy",
    )
    parser.add_argument("--max-water-level", type=float, default=95.0, help="High water mark percentage")
    parser.add_argument("--min-water-level", type=float, default=90.0, help="Low water mark percentage")
    parser.add_argument("--hot-block-percent", type=float, default=20.0, help="Percentage of hot blocks")
    parser.add_argument("--hot-read-percent", type=float, default=80.0, help="Percentage of reads targeting hot blocks")
    parser.add_argument("--reads-per-allocation", type=int, default=4, help="Reads per allocation attempt")
    parser.add_argument("--sample-every", type=int, default=100000, help="Snapshot interval in steps")
    parser.add_argument("--seed", type=int, default=123, help="Random seed")
    parser.add_argument("--workload", type=str, default="water-level", help="Workload mode")
    parser.add_argument("--json-report-path", type=str, default=None, help="Optional path to save JSON report")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = run_simulation(args)
        if args.json_report_path:
            with open(args.json_report_path, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=2)
        print_report(report)
        return 0
    except (ValueError, RuntimeError, OSError, ImportError, subprocess.CalledProcessError) as error:
        print(f"Measurement aborted: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
