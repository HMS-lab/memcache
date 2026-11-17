# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

import os
import subprocess
from collections.abc import Iterator
from hashlib import sha256
from pathlib import Path

import pytest
from pandas import DataFrame, isna
from pytest import CaptureFixture, MonkeyPatch, TempPathFactory

import meta_manager_sim.simulation as simulation
from meta_manager_sim.config import MetaManagerConfig, RunConfig
from meta_manager_sim.metrics import read_rss, render_summary
from meta_manager_sim.simulation import _simulate_reads

pytestmark = pytest.mark.native
BLOCK_SIZE = 4096
ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def native_key_tool(tmp_path_factory: TempPathFactory) -> Path:
    executable = tmp_path_factory.mktemp("native-key") / "native_key_test"
    subprocess.run(
        [
            "c++",
            "-std=c++17",
            "-I",
            str(ROOT / "src/meta_manager_sim/native/src"),
            str(ROOT / "tests/native_key_test.cpp"),
            "-lcrypto",
            "-o",
            str(executable),
        ],
        check=True,
        timeout=30,
    )
    return executable


@pytest.mark.parametrize(
    "key,model,prefix",
    [
        ("example-model:12", "example-model", "example_model_12_tp_0_pp_0"),
        ("example_model:12", "example_model", "example_model_12_tp_0_pp_0"),
        ("org:model:1", "org:model", "org:model_1_tp_0_pp_0"),
        ("bare", "", "_bare_tp_0_pp_0"),
        ("módèl-模型:42", "módèl-模型", "módèl_模型_42_tp_0_pp_0"),
    ],
)
def test_native_key_matches_python_format(
    native_key_tool: Path, key: str, model: str, prefix: str
) -> None:
    result = subprocess.run(
        [native_key_tool, key],
        capture_output=True,
        text=True,
        check=True,
        timeout=30,
    )
    expected = f"{prefix}_{sha256(prefix.encode('utf-8')).hexdigest()}"
    assert result.stdout.splitlines() == [model, expected]


@pytest.fixture(scope="module")
def measured_metrics() -> dict[str, DataFrame]:
    reads = [
        ("org:model:1", 1),
        ("org:model:1", 1),
        ("org:model:2", 8192),
        ("org:model:2", 8192),
        ("other:1", 131072),
        ("other:1", 131072),
        ("third:1", 4096),
        ("bare", 4096),
        ("bare", 4096),
    ]
    return _simulate_reads(
        iter(reads),
        RunConfig(
            MetaManagerConfig(((1, 65536),), max_water_level=100),
            detailed_stats=True,
            memory_sample_every=2,
        ),
    )


def test_read_outcomes_and_samples(
    measured_metrics: dict[str, DataFrame],
) -> None:
    summary = measured_metrics["summary"].iloc[0]
    assert (summary["reads"], summary["hits"], summary["misses"]) == (9, 3, 6)
    assert (summary["alloc_ok"], summary["alloc_fail"]) == (4, 2)
    assert summary["read_bytes"] == 290818
    assert summary["hit_bytes"] == 12289
    assert summary["fail_bytes"] == 262144
    assert summary["hit_rate"] == pytest.approx(1 / 3)
    assert summary["byte_hit_rate"] == pytest.approx(12289 / 290818)
    assert summary["alloc_fail_rate"] == pytest.approx(1 / 3)
    assert summary["evict_checks"] == summary["alloc_ok"] == 4
    assert summary["cached_keys"] == 4
    assert summary["evicted"] == 0
    assert summary["pool_used_bytes"] == 20480
    assert summary["pool_logical_bytes"] == 16385
    assert summary["pool_padding_bytes"] == 4095
    memory = measured_metrics["memory"]
    assert memory["reads"].tolist() == [0, 2, 4, 6, 8, 9]
    assert (memory["rss_bytes"] > 0).all()


def test_prefix_size_and_timing_breakdowns(
    measured_metrics: dict[str, DataFrame], capsys: CaptureFixture[str]
) -> None:
    summary = measured_metrics["summary"].iloc[0]
    prefixes = measured_metrics["prefixes"]
    assert set(prefixes.index) == {"org:model", "other", "third", ""}
    assert prefixes.at["org:model", "hit_rate"] == 0.5
    assert prefixes.at["other", "alloc_fail"] == 2
    assert prefixes.at["", "hits"] == 1
    for field in (
        "reads",
        "hit_bytes",
        "alloc_ok",
        "alloc_fail",
        "fail_bytes",
    ):
        assert prefixes[field].sum() == summary[field]

    sizes = measured_metrics["allocations"]
    assert sizes.index.tolist() == [1, 4096, 8192, 131072]
    assert sizes.at[1, "alloc_reserved_bytes"] == BLOCK_SIZE
    assert sizes.at[4096, "alloc_ok"] == 2
    assert sizes.at[131072, "alloc_fail"] == 2
    assert sizes.at[131072, "fail_bytes"] == 262144
    assert sizes.at[131072, "alloc_fail_rate"] == 1
    timings = measured_metrics["latencies"]
    assert timings["count"].to_dict() == {
        "lookup": 9,
        "alloc": 6,
        "write_finish": 4,
        "read_finish": 3,
        "evict_check": 4,
    }
    assert (timings["min_ns"] <= timings["mean_ns"]).all()
    assert (timings["mean_ns"] <= timings["max_ns"]).all()
    render_summary(measured_metrics)
    output = capsys.readouterr().out
    assert "33.33%" in output
    assert "org:model" in output


@pytest.mark.parametrize("policy", ["lru", "sharded_lru"])
def test_eviction_completes_after_each_allocation(policy: str) -> None:
    metrics = _simulate_reads(
        ((f"model:{index}", BLOCK_SIZE) for index in range(12)),
        RunConfig(
            MetaManagerConfig(
                ((1, 10 * BLOCK_SIZE),),
                min_water_level=20,
                max_water_level=50,
                eviction_policy=policy,
            )
        ),
    )
    summary = metrics["summary"].iloc[0]
    assert summary["alloc_ok"] == summary["evict_checks"] == 12
    assert summary["alloc_fail"] == 0
    assert summary["evicted"] > 0
    assert summary["cached_keys"] + summary["evicted"] == 12
    assert summary["pool_used_bytes"] == summary["cached_keys"] * BLOCK_SIZE
    assert summary["alloc_reserved_bytes"] > summary["pool_used_bytes"]
    assert metrics["memory"]["cached_keys"].iloc[1:5].tolist() == [1, 2, 3, 4]
    assert metrics["memory"]["cached_keys"].iloc[6] < 6


def test_sampling_with_unequal_pools() -> None:
    metrics = _simulate_reads(
        (("model:a", 2 * BLOCK_SIZE) for _ in range(5)),
        RunConfig(
            MetaManagerConfig(((1, BLOCK_SIZE), (1, 16 * BLOCK_SIZE))),
            memory_sample_every=2,
        ),
    )
    assert metrics["memory"]["reads"].tolist() == [0, 2, 4, 5]
    assert metrics["summary"].at[0, "hits"] == 4
    assert metrics["summary"].at[0, "evict_checks"] == 1
    assert metrics["pools"]["used_bytes"].to_dict() == {0: 0, 1: 8192}
    assert "prefixes" not in metrics
    assert "allocations" not in metrics
    assert "latencies" not in metrics


@pytest.mark.parametrize("detailed", [False, True])
def test_empty_workload_has_undefined_rates(
    detailed: bool, capsys: CaptureFixture[str]
) -> None:
    metrics = _simulate_reads(
        [], RunConfig(detailed_stats=detailed, memory_sample_every=0)
    )
    summary = metrics["summary"].iloc[0]
    assert summary["reads"] == summary["cached_keys"] == 0
    for field in (
        "hit_rate",
        "byte_hit_rate",
        "alloc_ok_rate",
        "alloc_fail_rate",
    ):
        assert isna(summary[field])
    assert metrics["memory"]["reads"].tolist() == [0, 0]
    render_summary(metrics)
    assert "n/a" in capsys.readouterr().out


def test_input_failure_reaps_native_worker(monkeypatch: MonkeyPatch) -> None:
    pids: list[int] = []

    def observe(pid: int) -> int:
        pids.append(pid)
        return read_rss(pid)

    def reads() -> Iterator[tuple[str, int]]:
        yield "model:a", BLOCK_SIZE
        raise ValueError("input failed")

    monkeypatch.setattr(simulation, "read_rss", observe)
    with pytest.raises(ValueError, match="input failed"):
        _simulate_reads(reads())
    assert pids and pids[0] != os.getpid()
    assert not Path(f"/proc/{pids[0]}").exists()
