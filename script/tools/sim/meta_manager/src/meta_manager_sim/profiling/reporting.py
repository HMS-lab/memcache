# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Render the same recorded metrics as terminal tables, JSON, or CSV."""

import csv
import json
from pathlib import Path
from typing import Any

from tabulate import tabulate

from meta_manager_sim.profiling.models import SimulationResult


def print_report(report: SimulationResult) -> None:
    metrics = report["metrics"]
    final = report["samples"][-1]
    hit_rate = metrics["cache_hit_rate_percent"]
    byte_hit_rate = metrics["byte_hit_rate_percent"]
    throughput = metrics["accesses_per_second"]
    summary: list[list[str | None]] = [
        ["Accesses", str(metrics["accesses"])],
        ["Cache hits", str(metrics["cache_hits"])],
        ["Cache misses", str(metrics["cache_misses"])],
        [
            "Cache hit rate (%)",
            f"{hit_rate:.3f}" if hit_rate is not None else None,
        ],
        [
            "Byte hit rate (%)",
            f"{byte_hit_rate:.3f}" if byte_hit_rate is not None else None,
        ],
        ["Successful allocations", str(metrics["allocation_successes"])],
        ["Failed allocations", str(metrics["allocation_failures"])],
        ["Key already exists", str(metrics["allocation_duplicates"])],
        ["Reads completed", str(metrics["read_completions"])],
        ["Writes completed", str(metrics["write_completions"])],
        ["Registered keys", str(final["registered_keys"])],
        ["Reclaimed backing blobs", str(final["reclaimed_blobs"])],
        ["Elapsed wall time (s)", f"{metrics['elapsed_wall_seconds']:.3f}"],
        [
            "Accesses / s",
            f"{throughput:.1f}" if throughput is not None else None,
        ],
    ]

    print(f"MetaManager {report['workload']['kind']} simulation")
    if report["workload"]["key_scope_override"]:
        print("Key scope: model:hash; trace declares local hashes")
    print("Native mutation safety: unresolved; exploratory results")
    print(
        tabulate(
            summary, tablefmt="simple", missingval="n/a", disable_numparse=[1]
        )
    )
    print()
    _print_allocations(metrics["by_size"])
    if metrics["by_model"]:
        print()
        _print_models(metrics["by_model"])
    print()
    _print_pools(final["pools"])
    print()
    _print_timings(metrics["api_timing"])


def _print_allocations(sizes: list[dict[str, Any]]) -> None:
    columns = {
        "size_bytes": "Block size (B)",
        "allocation_attempts": "Attempts",
        "allocation_successes": "Succeeded",
        "allocation_duplicates": "Already exists",
        "allocation_failures": "Failed",
        "allocation_failure_percent": "Failure (%)",
    }
    print("Allocations by requested block size")
    print(
        tabulate(
            [[row[field] for field in columns] for row in sizes],
            headers=list(columns.values()),
            floatfmt=".3f",
            missingval="n/a",
        )
    )


def _print_models(models: list[dict[str, Any]]) -> None:
    columns = {
        "model": "Model",
        "accesses": "Accesses",
        "cache_hit_rate_percent": "Cache hits (%)",
        "allocation_successes": "Allocated",
        "allocation_duplicates": "Already exists",
        "allocation_failures": "Failed",
        "allocation_failure_percent": "Failure (%)",
    }
    print("Results by model")
    print(
        tabulate(
            [[row[field] for field in columns] for row in models],
            headers=list(columns.values()),
            floatfmt=".3f",
            missingval="n/a",
        )
    )


def _print_pools(pools: list[dict[str, int | float | str | None]]) -> None:
    columns = {
        "rank": "Rank",
        "media": "Media",
        "capacity_bytes": "Capacity (B)",
        "reserved_bytes": "Reserved (B)",
        "free_bytes": "Free (B)",
        "padding_bytes": "Padding (B)",
        "padding_percent": "Padding (%)",
        "occupancy_percent": "Used (%)",
    }
    rows = [[pool[field] for field in columns] for pool in pools]
    print(
        tabulate(
            rows,
            headers=list(columns.values()),
            floatfmt=".3f",
            missingval="n/a",
        )
    )


def _print_timings(timings: dict[str, dict[str, int | float]]) -> None:
    labels = {
        "get": "Get",
        "alloc": "Alloc",
        "finish_read": "Complete read",
        "finish_write": "Complete write",
        "watermark_check": "Check eviction thresholds",
    }
    print(
        tabulate(
            [
                [
                    labels[name],
                    timing["calls"],
                    timing["total_seconds"],
                    timing["mean_ns"] / 1000,
                    timing["max_ns"] / 1000,
                ]
                for name, timing in timings.items()
            ],
            headers=[
                "Native API",
                "Calls",
                "Total (s)",
                "Mean (µs)",
                "Max (µs)",
            ],
            floatfmt=".3f",
        )
    )


def write_reports(
    report: SimulationResult, json_path: Path | None, csv_path: Path | None
) -> None:
    if json_path is not None:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = json_path.with_name(json_path.name + ".tmp")
        temporary.write_text(
            json.dumps(
                report, indent=2, allow_nan=False, default=_json_default
            )
            + "\n",
            encoding="utf-8",
        )
        temporary.replace(json_path)
    if csv_path is not None:
        csv_path.mkdir(parents=True, exist_ok=True)
        tables = {
            "summary": [
                {
                    name: value
                    for name, value in report["metrics"].items()
                    if value is None or isinstance(value, (int, float, str))
                }
            ],
            "sizes": [
                {key: value for key, value in row.items() if key != "outcomes"}
                for row in report["metrics"]["by_size"]
            ],
            "models": [
                {key: value for key, value in row.items() if key != "outcomes"}
                for row in report["metrics"]["by_model"]
            ],
            "memory": [
                {
                    "accesses": sample["accesses"],
                    "registered_keys": sample["registered_keys"],
                    "virtual_time_ns": sample["virtual_time_ns"],
                    "reason": sample["reason"],
                    "requested_threshold": sample["requested_threshold"],
                    **pool,
                }
                for sample in report["samples"]
                for pool in sample["pools"]
            ],
            "api_timings": [
                {"api": name, **values}
                for name, values in report["metrics"]["api_timing"].items()
            ],
        }
        for name, rows in tables.items():
            path = csv_path / f"{name}.csv"
            temporary = path.with_suffix(".tmp")
            with temporary.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(
                    stream,
                    fieldnames=(
                        list(rows[0])
                        if rows
                        else (
                            ["size_bytes" if name == "sizes" else "model"]
                            if name in ("sizes", "models")
                            else [
                                "api",
                                "calls",
                                "total_ns",
                                "min_ns",
                                "max_ns",
                                "mean_ns",
                                "total_seconds",
                            ]
                        )
                    ),
                )
                writer.writeheader()
                writer.writerows(rows)
            temporary.replace(path)


def _json_default(value: object) -> str:
    if isinstance(value, Path):
        return str(value)
    raise TypeError(f"Cannot serialize {type(value).__name__}")
