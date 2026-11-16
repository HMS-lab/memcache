# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from collections.abc import Iterable
from dataclasses import asdict
from hashlib import sha256
from typing import cast

from pandas import DataFrame

from .config import RunConfig
from .metrics import plot_memory, read_rss, render_summary, summarize_metrics
from .native import NativeProcess

""" ----------------------------------------------------------------------- """


def _initialize_manager(
    process: NativeProcess, config: RunConfig
) -> dict[str, object]:
    # Configure MetaManager and return its PID and initial state
    return process.request(
        "init",
        config={
            **asdict(config.manager_cfg),
            # Note: Convert pool tuples to the native format (overwrite)
            "pools": [
                {"count": count, "capacity_bytes": capacity}
                for count, capacity in config.manager_cfg.pools
            ],
        },
        detailed_stats=config.detailed_stats,
    )


def _request_reads(
    process: NativeProcess,
    reads: Iterable[tuple[str, int]],
    snapshot_init: dict[str, object],
    sample_interval: int,
) -> tuple[dict[str, object], list[tuple[int, int, int]]]:
    samples: list[tuple[int, int, int]] = []

    def _sample(progress: dict[str, object]) -> None:
        # Note: Use the C++ PID even when Heaptrack launches the process
        samples.append(
            (
                cast(int, progress["reads"]),
                read_rss(cast(int, snapshot_init["pid"])),
                cast(int, progress["cached_keys"]),
            )
        )

    # Record starting RSS before consuming the workload
    _sample(cast(dict[str, object], snapshot_init["progress"]))

    for reads_completed, (key, size) in enumerate(reads, start=1):
        # Recreate the key's native format for more accurate measurements
        model, _, block_id = key.rpartition(":")
        prefix = f"{model.replace('-', '_')}_{block_id}_tp_0_pp_0"
        native_key = f"{prefix}_{sha256(prefix.encode('utf-8')).hexdigest()}"

        progress = process.request(
            "read", reads=[(native_key, size)], model=model
        )
        if sample_interval and reads_completed % sample_interval == 0:
            _sample(progress)

    # Collect the final state and RSS before the native process shuts down
    snapshot_final = process.request("snapshot")
    _sample(cast(dict[str, object], snapshot_final["progress"]))

    return snapshot_final, samples


def _build_metrics(
    snapshot: dict[str, object], samples: list[tuple[int, int, int]]
) -> dict[str, DataFrame]:
    metrics = {
        "summary": DataFrame([snapshot["progress"]], dtype="uint64"),
        "memory": DataFrame.from_records(
            samples, columns=["reads", "rss_bytes", "cached_keys"]
        ),
        "pools": DataFrame(
            cast(list[dict[str, int]], snapshot["pools"]),
            columns=["rank", "capacity_bytes", "used_bytes", "padding_bytes"],
            dtype="uint64",
        ).set_index("rank"),
    }

    if (details := snapshot["details"]) is not None:
        details = cast(dict[str, dict[str, dict[str, object]]], details)

        # Access counters (by key prefix), excluding globally scoped fields
        access_columns = metrics["summary"].columns.drop(
            ["evict_checks", "cached_keys", "evicted"]
        )
        metrics["prefixes"] = DataFrame.from_dict(
            details.get("prefixes", {}),
            orient="index",
            columns=access_columns,
            dtype="uint64",
        ).rename_axis("prefix")

        # Allocation outcomes and byte counts (by block size)
        by_size = DataFrame.from_dict(
            details.get("allocations", {}),
            orient="index",
            columns=[
                "allocs",
                "alloc_ok",
                "alloc_fail",
                "request_bytes",
                "alloc_bytes",
                "alloc_reserved_bytes",
                "fail_bytes",
            ],
            dtype="uint64",
        ).rename_axis("size_bytes")
        # Note: JSON object keys are strings
        by_size.index = by_size.index.astype("uint64")
        metrics["allocations"] = by_size.sort_index()

        # Native call counts and timings (by operation)
        metrics["latencies"] = DataFrame.from_dict(
            details.get("latencies", {}),
            orient="index",
            columns=["count", "total_ns", "min_ns", "max_ns"],
            dtype="uint64",
        ).rename_axis("operation")

    return metrics


def _simulate_reads(
    reads: Iterable[tuple[str, int]], config: RunConfig = RunConfig()
) -> dict[str, DataFrame]:
    with NativeProcess(config.profile) as process:
        # Initialize MetaManager and replay reads sequentially
        snapshot_init = _initialize_manager(process, config)
        snapshot_final, samples = _request_reads(
            process, reads, snapshot_init, config.memory_sample_every
        )

        profile_path = process.close()

    metrics = _build_metrics(snapshot_final, samples)
    metrics["summary"] = metrics["summary"].assign(
        detailed_stats=config.detailed_stats,
        profile_path=str(profile_path) if profile_path else None,
    )

    # Aggregate rates, memory statistics and native operation latencies
    summarize_metrics(metrics)

    return metrics


""" ----------------------------------------------------------------------- """


def run_simulation(
    reads: Iterable[tuple[str, int]], config: RunConfig
) -> None:
    metrics = _simulate_reads(reads, config)
    render_summary(metrics)

    # Save RSS/cached-key plots against completed reads
    if config.memory_plot:
        plot_memory(metrics["memory"], config.memory_plot)
        print(f"Saved memory plot at: {config.memory_plot}")

    # Output the Heaptrack capture path if profiling was enabled
    profile_path = metrics["summary"].at[0, "profile_path"]
    if profile_path:
        print(f"Saved Heaptrack capture at: {profile_path}")
