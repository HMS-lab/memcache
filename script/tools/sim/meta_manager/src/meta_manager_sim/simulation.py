# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from collections.abc import Iterable
from dataclasses import asdict
from typing import cast

from pandas import DataFrame

from .config import RunConfig
from .metrics import plot_memory, read_rss
from .metrics.summary import (
    print_latencies,
    print_memory,
    render_summary,
    summarize_metrics,
)
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
            # Note: Convert pool tuples to the native format
            # (overwrite `config.manager_cfg.pools`)
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
    config: RunConfig,
) -> tuple[
    dict[str, object], list[dict[str, list[int]]], list[tuple[int, int, int]]
]:
    latencies: list[dict[str, list[int]]] = []
    memory_samples: list[tuple[int, int, int]] = []

    def _sample(progress: dict[str, object]) -> None:
        # Note: Use the C++ PID even when Heaptrack launches the process
        memory_samples.append(
            (
                cast(int, progress["reads"]),
                read_rss(cast(int, snapshot_init["pid"])),
                cast(int, progress["cached_keys"]),
            )
        )

    # Record starting RSS before consuming the workload
    _sample(cast(dict[str, object], snapshot_init["progress"]))

    for reads_completed, read in enumerate(reads, start=1):
        progress = process.request("read", reads=[read])

        # Note: Hits and misses produce different sets of timings,
        # so keep one dictionary per read to slice by read count
        if config.detailed_stats:
            latencies.append(
                cast(dict[str, list[int]], progress.pop("latencies"))
            )

        if (
            config.memory_sample_interval
            and reads_completed % config.memory_sample_interval == 0
        ):
            _sample(progress)

    # Collect the final state and RSS before the native process shuts down
    snapshot_final = process.request("snapshot")
    _sample(cast(dict[str, object], snapshot_final["progress"]))

    return snapshot_final, latencies, memory_samples


def _memory_frame(samples: list[tuple[int, int, int]]) -> DataFrame:
    return DataFrame.from_records(
        samples, columns=["reads", "rss_bytes", "cached_keys"]
    )


def _latency_frame(latencies: list[dict[str, list[int]]]) -> DataFrame:
    return (
        DataFrame.from_records(
            (
                (operation, duration)
                for timings in latencies
                for operation, values in timings.items()
                for duration in values
            ),
            columns=["operation", "latency_ns"],
        )
        .set_index("operation")
        .astype("uint64")
    )


def _build_metrics(
    snapshot: dict[str, object],
    latencies: list[dict[str, list[int]]],
    memory_samples: list[tuple[int, int, int]],
) -> dict[str, DataFrame]:
    metrics = {
        "summary": DataFrame([snapshot["progress"]], dtype="uint64"),
        "memory": _memory_frame(memory_samples),
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

        # Individual durations, grouped by operation during summarization
        metrics["latencies"] = _latency_frame(latencies)

    return metrics


def _simulate_reads(
    reads: Iterable[tuple[str, int]], config: RunConfig = RunConfig()
) -> dict[str, DataFrame]:
    with NativeProcess(config.profile) as process:
        # Initialize MetaManager and replay reads sequentially
        snapshot_init = _initialize_manager(process, config)
        snapshot_final, latencies, memory_samples = _request_reads(
            process, reads, snapshot_init, config
        )

        profile_path = process.close()

    metrics = _build_metrics(snapshot_final, latencies, memory_samples)
    metrics["summary"] = metrics["summary"].assign(
        detailed_stats=config.detailed_stats,
        profile_path=str(profile_path) if profile_path else None,
    )

    # Aggregate rates, memory statistics and native operation latencies
    summarize_metrics(metrics)

    # Print latency statistics for each separate read interval
    if config.latency_sample_interval:
        for start in range(0, len(latencies), config.latency_sample_interval):
            window = latencies[start : start + config.latency_sample_interval]
            print_latencies(
                _latency_frame(window), start + 1, start + len(window)
            )

    # Print memory statistics between consecutive RSS samples
    if config.memory_sample_interval:
        for index in range(1, len(memory_samples)):
            # Note: The final snapshot may repeat the last sampled read count
            if memory_samples[index - 1][0] != memory_samples[index][0]:
                print_memory(
                    _memory_frame(memory_samples[index - 1 : index + 1])
                )

    return metrics


""" ----------------------------------------------------------------------- """


def run_simulation(
    reads: Iterable[tuple[str, int]], config: RunConfig
) -> None:
    metrics = _simulate_reads(reads, config)
    print("======== SUMMARY ========")
    render_summary(metrics)

    # Save "RSS/cached-key against completed reads" plots
    if config.memory_plot:
        plot_memory(metrics["memory"], config.memory_plot)
        print(f"Saved memory plot at: {config.memory_plot}")

    # Output the Heaptrack capture path
    profile_path = metrics["summary"].at[0, "profile_path"]
    if profile_path:
        print(f"Saved Heaptrack capture at: {profile_path}")
