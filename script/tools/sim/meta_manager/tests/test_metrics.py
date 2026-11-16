# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

from io import StringIO
from pathlib import Path

import matplotlib.pyplot as plt
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


def test_rss_is_read_from_the_requested_pid_in_bytes(
    monkeypatch: MonkeyPatch,
) -> None:
    def open_smaps(path: Path) -> StringIO:
        assert path == Path("/proc/123/smaps_rollup")
        return StringIO("header\nPss: 2 kB\nRss: 42 kB\nRssAnon: 12 kB")

    monkeypatch.setattr(Path, "open", open_smaps)
    assert read_rss(123) == 42 * 1024


def test_exact_totals_and_decreasing_rss(capsys: CaptureFixture[str]) -> None:
    large = (1 << 53) + 1
    metrics = {
        "summary": DataFrame(
            [
                {
                    "reads": large,
                    "hits": large - 1,
                    "misses": 1,
                    "read_bytes": large,
                    "hit_bytes": large - 1,
                    "allocs": 1,
                    "alloc_ok": 1,
                    "alloc_fail": 0,
                    "cached_keys": 1,
                    "evicted": 0,
                    "detailed_stats": False,
                }
            ]
        ),
        "pools": DataFrame(
            {
                "capacity_bytes": [(1 << 63) + 1, (1 << 63) + 1],
                "used_bytes": [4096, 0],
                "padding_bytes": [4095, 0],
            },
            dtype="uint64",
        ),
        "memory": DataFrame({"rss_bytes": [32768, 24576]}),
    }
    summarize_metrics(metrics)
    summary = metrics["summary"]
    assert summary.at[0, "reads"] == large
    assert summary.at[0, "pool_capacity_bytes"] == (1 << 64) + 2
    assert summary.at[0, "pool_logical_bytes"] == 1
    assert summary.at[0, "rss_delta_bytes"] == -8192
    assert summary.at[0, "rss_max_bytes"] == 32768

    originals = {name: frame.copy() for name, frame in metrics.items()}
    render_summary(metrics)
    output = capsys.readouterr().out
    assert str(large) in output
    assert "100.00%" in output
    for name, original in originals.items():
        assert_frame_equal(metrics[name], original)


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
