# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from pandas import DataFrame

""" ----------------------------------------------------------------------- """


def _summarize_rates(metrics: dict[str, DataFrame]) -> None:
    for name in ("summary", "prefixes"):
        if (counters := metrics.get(name)) is not None:
            counters["hit_rate"] = counters["hits"] / counters["reads"]
            counters["byte_hit_rate"] = (
                counters["hit_bytes"] / counters["read_bytes"]
            )

    for name in ("summary", "prefixes", "allocations"):
        if (counters := metrics.get(name)) is not None:
            counters["alloc_ok_rate"] = (
                counters["alloc_ok"] / counters["allocs"]
            )
            counters["alloc_fail_rate"] = (
                counters["alloc_fail"] / counters["allocs"]
            )


def _summarize_pools(summary: DataFrame, pools: DataFrame) -> None:
    pools["logical_bytes"] = pools["used_bytes"] - pools["padding_bytes"]
    pools["free_bytes"] = pools["capacity_bytes"] - pools["used_bytes"]
    pools["usage"] = pools["used_bytes"] / pools["capacity_bytes"]

    for column in (
        "capacity_bytes",
        "used_bytes",
        "logical_bytes",
        "free_bytes",
        "padding_bytes",
    ):
        # Note: Python sums preserve large integer byte counts
        summary[f"pool_{column}"] = sum(pools[column].tolist())
    summary["pool_usage"] = (
        summary["pool_used_bytes"] / summary["pool_capacity_bytes"]
    )


def _summarize_memory(summary: DataFrame, memory: DataFrame) -> None:
    summary["rss_start_bytes"] = memory["rss_bytes"].iloc[0]
    summary["rss_end_bytes"] = memory["rss_bytes"].iloc[-1]
    summary["rss_delta_bytes"] = (
        summary["rss_end_bytes"] - summary["rss_start_bytes"]
    )
    summary["rss_max_bytes"] = memory["rss_bytes"].max()


def _overview_table(summary: DataFrame) -> DataFrame:
    overview = summary[
        [
            "hit_rate",
            "byte_hit_rate",
            "reads",
            "hits",
            "misses",
            "alloc_ok",
            "alloc_fail",
            "alloc_fail_rate",
            "cached_keys",
            "evicted",
            "pool_usage",
            "rss_start_bytes",
            "rss_end_bytes",
            "rss_max_bytes",
            "rss_delta_bytes",
        ]
    ].copy()

    # Convert RSS to MiB for display
    rss_columns = overview.filter(like="rss_").columns
    overview[rss_columns] = overview[rss_columns] / (1 << 20)
    overview.columns = overview.columns.str.replace("_bytes", "_MiB")

    return overview


def _detail_tables(metrics: dict[str, DataFrame]) -> dict[str, DataFrame]:
    return {
        "Pools": metrics["pools"][["capacity_bytes", "used_bytes", "usage"]],
        "By prefix": metrics["prefixes"][
            ["reads", "hit_rate", "byte_hit_rate", "alloc_ok", "alloc_fail"]
        ],
        "By size": metrics["allocations"][
            ["allocs", "alloc_ok", "alloc_fail", "alloc_fail_rate"]
        ],
        "Timings": metrics["latencies"][["count", "mean_ns", "max_ns"]],
    }


def _table_to_str(frame: DataFrame, vertical: bool = False) -> str:
    display = frame.copy()

    # Show rates and pool usage as percentages
    for column in display.columns:
        if column.endswith("rate") or column.endswith("usage"):
            display[column] = display[column].map(
                "{:.2%}".format, na_action="ignore"
            )

    display.columns = display.columns.str.replace("_", " ")
    if vertical:
        # Note: Preserve integer counts when transposing mixed numeric columns
        display = display.astype(object).T
        display.columns = ["Value"]

    return display.to_string(na_rep="n/a", float_format="{:.6f}".format)


""" ----------------------------------------------------------------------- """


def summarize_metrics(metrics: dict[str, DataFrame]) -> None:
    summary = metrics["summary"]

    _summarize_rates(metrics)
    _summarize_pools(summary, metrics["pools"])
    _summarize_memory(summary, metrics["memory"])

    if "latencies" in metrics:
        latencies = metrics["latencies"]
        latencies["mean_ns"] = latencies["total_ns"] / latencies["count"]


def render_summary(metrics: dict[str, DataFrame]) -> None:
    # Start with the overview, then include detailed stats if present
    frames = {"Overview": _overview_table(metrics["summary"])}
    if metrics["summary"].at[0, "detailed_stats"]:
        frames.update(_detail_tables(metrics))

    print(
        "\n\n".join(
            f"{name}\n{_table_to_str(frame, vertical=name == 'Overview')}"
            for name, frame in frames.items()
            if not frame.empty
        )
    )
