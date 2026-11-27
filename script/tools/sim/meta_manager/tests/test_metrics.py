# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from io import StringIO
from pathlib import Path

import matplotlib.pyplot as plt
import pytest
from matplotlib.figure import Figure
from matplotlib.image import imread
from pandas import DataFrame
from pandas.testing import assert_frame_equal
from pytest import CaptureFixture, MonkeyPatch

from meta_manager_sim.metrics import (
    plot_memory,
    read_rss,
    render_summary,
    summarize_metrics,
)
from meta_manager_sim.metrics.summary import (
    _summarize_latencies,
    print_latencies,
    print_memory,
)


def test_rss_is_read_from_the_requested_pid_in_bytes(
    monkeypatch: MonkeyPatch,
) -> None:
    def open_smaps(path: Path) -> StringIO:
        assert path == Path("/proc/123/smaps_rollup")
        return StringIO("header\nPss: 2 kB\nRss: 42 kB\nRssAnon: 12 kB")

    monkeypatch.setattr(Path, "open", open_smaps)
    assert read_rss(123) == 42 * 1024


# Final RSS gives 12 KiB per key for two cached keys and n/a for zero keys
@pytest.mark.parametrize(
    ("cached_keys", "rss_per_key"), [(2, "12.000000"), (0, "n/a")]
)
def test_exact_totals_and_decreasing_rss(
    capsys: CaptureFixture[str], cached_keys: int, rss_per_key: str
) -> None:
    large = (1 << 53) + 1
    metrics = {
        "summary": DataFrame(
            [
                {
                    "reads": large,
                    "hits": large - 2,
                    "misses": 2,
                    "read_bytes": large,
                    "hit_bytes": large - 2,
                    "allocs": 2,
                    "alloc_ok": 2,
                    "alloc_fail": 0,
                    "cached_keys": cached_keys,
                    "evicted": 2 - cached_keys,
                    "detailed_stats": False,
                }
            ]
        ),
        "pools": DataFrame(
            {
                "capacity_bytes": [(1 << 63) + 1, (1 << 63) + 1],
                "used_bytes": [4096 * cached_keys, 0],
                "padding_bytes": [4095 * cached_keys, 0],
            },
            dtype="uint64",
        ),
        "memory": DataFrame({"rss_bytes": [32768, 24576]}),
    }
    summarize_metrics(metrics)
    summary = metrics["summary"]
    assert summary.at[0, "reads"] == large
    assert summary.at[0, "pool_capacity_bytes"] == (1 << 64) + 2
    assert summary.at[0, "pool_logical_bytes"] == cached_keys
    assert summary.at[0, "rss_delta_bytes"] == -8192
    assert summary.at[0, "rss_max_bytes"] == 32768

    originals = {name: frame.copy() for name, frame in metrics.items()}
    render_summary(metrics)
    output = capsys.readouterr().out
    assert str(large) in output
    assert "100.00%" in output
    assert [
        row.split()[-1]
        for row in output.splitlines()
        if row.startswith("RSS/key (KiB)")
    ] == [rss_per_key]
    for name, original in originals.items():
        assert_frame_equal(metrics[name], original)


def test_latency_summary_from_individual_samples() -> None:
    latencies = (
        DataFrame.from_records(
            [("lookup", value) for value in (10, 20, 30, 40, 50)]
            + [("alloc", 7)],
            columns=["operation", "latency_ns"],
        )
        .set_index("operation")
        .astype("uint64")
    )

    timings = _summarize_latencies(latencies)
    assert timings.loc["lookup"].to_dict() == pytest.approx(
        {
            "count": 5,
            "total": 150,
            "min": 10,
            "mean": 30,
            "max": 50,
            "p50": 30,
            "p90": 46,
            "p95": 48,
            "p99": 49.6,
            "p99.9": 49.96,
            "p99.99": 49.996,
        }
    )
    assert timings.at["alloc", "count"] == 1
    assert timings.loc["alloc"].drop("count").eq(7).all()
    for column in ("count", "total", "min", "max"):
        assert timings[column].dtype.kind in "iu"


def test_interval_printers_preserve_raw_data(
    capsys: CaptureFixture[str],
) -> None:
    memory = DataFrame({"reads": [3, 6], "rss_bytes": [3 << 20, 1 << 20]})
    latencies = DataFrame(
        {"latency_ns": [1000, 3000]},
        index=["lookup", "lookup"],
        dtype="uint64",
    ).rename_axis("operation")
    original_memory, original_latencies = memory.copy(), latencies.copy()

    print_memory(memory)
    print_latencies(latencies, 4, 6)
    output = capsys.readouterr().out
    assert "Memory [reads 3 to 6]" in output
    assert "Timings [ns, reads 4 to 6]" in output
    assert "MiB" in output and "p99.99" in output
    assert_frame_equal(memory, original_memory)
    assert_frame_equal(latencies, original_latencies)


def test_plot_exports_rss_and_cached_keys_against_reads(
    tmp_path: Path, monkeypatch: MonkeyPatch
) -> None:
    samples = DataFrame(
        {
            "reads": [0, 2, 4],
            "rss_bytes": [2 << 20, 3 << 20, 1 << 20],
            "cached_keys": [0, 1, 0],
        }
    )
    figures: list[Figure] = []
    savefig = Figure.savefig

    def save(figure: Figure, path: Path, **kwargs: object) -> None:
        figures.append(figure)
        savefig(figure, path, **kwargs)

    monkeypatch.setattr(Figure, "savefig", save)
    path = tmp_path / "plots/memory.png"
    plot_memory(samples, path)
    assert imread(path).std() > 0
    memory, keys = figures[0].axes
    assert memory.collections[0].get_offsets().tolist() == [
        [0, 2],
        [2, 3],
        [4, 1],
    ]
    assert keys.collections[0].get_offsets().tolist() == [
        [0, 0],
        [2, 1],
        [4, 0],
    ]
    assert keys.get_xlabel() == "Completed reads"
    assert not plt.get_fignums()
