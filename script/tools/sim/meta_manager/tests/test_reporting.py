# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
"""Reports preserve native outcomes and distinguish counts from bytes."""

import csv
import json
from pathlib import Path

import pytest

from meta_manager_sim.config import ManagerConfig
from meta_manager_sim.profiling.reporting import print_report, write_reports
from meta_manager_sim.simulation import run_simulation
from meta_manager_sim.workloads.trace.trace_workload import (
    TraceConfig,
    prepare_trace,
)


@pytest.mark.native
def test_trace_metrics_in_terminal_json_and_csv(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    trace = Path(__file__).resolve().parents[1] / "examples" / "traces.jsonl"
    config = TraceConfig(
        trace, (("example-model-a", 4096), ("example-model-b", 8192))
    )
    with prepare_trace(config) as workload:
        report = run_simulation(workload, ManagerConfig(capacity_bytes=65536))

    metrics = report["metrics"]
    assert report["schema_version"] == 3
    assert metrics["accesses"] == 17
    assert metrics["cache_hits"] == 10
    assert metrics["cache_misses"] == 7
    assert metrics["allocation_successes"] == 7
    assert metrics["allocation_success_percent"] == 100
    assert metrics["allocation_failures"] == 0
    assert metrics["requested_access_bytes"] == 94208
    assert metrics["cache_hit_bytes"] == 53248
    assert metrics["byte_hit_rate_percent"] == pytest.approx(100 * 13 / 23)
    assert metrics["cumulative_allocation_reserved_bytes"] == 40960
    assert metrics["cumulative_allocation_padding_bytes"] == 0
    assert report["samples"][-1]["registered_keys"] == 7

    write_reports(report, tmp_path / "report.json", tmp_path / "csv")
    decoded = json.loads((tmp_path / "report.json").read_text())
    assert decoded["metrics"] == metrics
    with (tmp_path / "csv" / "summary.csv").open(newline="") as stream:
        summary = next(csv.DictReader(stream))
    assert summary["allocation_successes"] == "7"
    assert summary["cache_miss_bytes"] == "40960"
    with (tmp_path / "csv" / "models.csv").open(newline="") as stream:
        models = {row["model"]: row for row in csv.DictReader(stream)}
    assert models["example-model-a"]["accesses"] == "11"
    assert models["example-model-a"]["cache_hits"] == "7"
    assert models["example-model-b"]["accesses"] == "6"
    assert models["example-model-b"]["cache_hits"] == "3"
    with (tmp_path / "csv" / "memory.csv").open(newline="") as stream:
        final = list(csv.DictReader(stream))[-1]
    assert final["reserved_bytes"] == "40960"
    assert final["padding_bytes"] == "0"

    print_report(report)
    output = capsys.readouterr().out
    assert "Successful allocations" in output
    assert "Already exists" in output
    assert "Results by model" in output
    assert "published" not in output.lower()
